/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2020 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#ifndef OSGEARTH_GLTF_READER_H
#define OSGEARTH_GLTF_READER_H

#include <OpenThreads/ScopedLock>
#include <osg/Node>
#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgDB/FileNameUtils>
#include <osgDB/ReaderWriter>
#include <osgDB/FileNameUtils>
#include <osgUtil/SmoothingVisitor>
#include <osgEarth/Notify>
#include <osgEarth/URI>
#include <osgEarth/Containers>
#include <osgEarth/Registry>
#include <osgEarth/ShaderUtils>


#undef LC
#define LC "[GLTFWriter] "

class GLTFReader
{
public:
    typedef osgEarth::Threading::Lockable<
        osgEarth::UnorderedMap<std::string, osg::ref_ptr<osg::Texture2D> > 
    > TextureCache;

    struct NodeBuilder;

    static std::string ExpandFilePath(const std::string &filepath, void * userData)
    {
        const std::string& referrer = *(const std::string*)userData;
        std::string path = osgDB::getRealPath(osgDB::isAbsolutePath(filepath) ? filepath : osgDB::concatPaths(osgDB::getFilePath(referrer), filepath));
        OSG_NOTICE << "ExpandFilePath: expanded " << filepath << " to " << path << std::endl;
        return tinygltf::ExpandFilePath(path, userData);
    }

    struct Env
    {
        Env(const std::string& loc, const osgDB::Options* opt) : referrer(loc), readOptions(opt) { }
        const std::string referrer;
        const osgDB::Options* readOptions;
    };

public:
    mutable TextureCache* _texCache;

    GLTFReader() : _texCache(NULL)
    {
        //NOP
    }

    void setTextureCache(TextureCache* cache) const
    {
        _texCache = cache;
    }

    osgDB::ReaderWriter::ReadResult read(const std::string& location,
                                         bool isBinary,
                                         const osgDB::Options* readOptions) const
    {
        std::string err, warn;
        tinygltf::Model model;
        tinygltf::TinyGLTF loader;

        tinygltf::FsCallbacks fs;
        fs.FileExists = &tinygltf::FileExists;
        fs.ExpandFilePath = &GLTFReader::ExpandFilePath;
        fs.ReadWholeFile = &tinygltf::ReadWholeFile;
        fs.WriteWholeFile = &tinygltf::WriteWholeFile;
        fs.user_data = (void*)&location;
        loader.SetFsCallbacks(fs);

        tinygltf::Options opt;
        opt.skip_imagery = readOptions && readOptions->getOptionString().find("gltfSkipImagery") != std::string::npos;

        if (osgDB::containsServerAddress(location))
        {
            osgEarth::ReadResult rr = osgEarth::URI(location).readString(readOptions);
            if (rr.failed())
            {
                return osgDB::ReaderWriter::ReadResult::FILE_NOT_FOUND;
            }

            std::string mem = rr.getString();

            if (isBinary)
            {
                loader.LoadBinaryFromMemory(&model, &err, &warn, (const unsigned char*)mem.data(), mem.size(), location, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromString(&model, &err, &warn, mem.data(), mem.size(), location, REQUIRE_VERSION, &opt);
            }
        }
        else
        {
            if (isBinary)
            {
                loader.LoadBinaryFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
            else
            {
                loader.LoadASCIIFromFile(&model, &err, &warn, location, REQUIRE_VERSION, &opt);
            }
        }

        if (!err.empty()) {
            OE_WARN << LC << "gltf Error loading " << location << std::endl;
            OE_WARN << LC << err << std::endl;
            return osgDB::ReaderWriter::ReadResult::ERROR_IN_READING_FILE;
        }

        Env env(location, readOptions);
        return makeNodeFromModel(model, env);
    }

    osg::Node* makeNodeFromModel(const tinygltf::Model &model, const Env& env) const
    {
        NodeBuilder builder(this, model, env);
        // Rotate y-up to z-up
        osg::MatrixTransform* transform = new osg::MatrixTransform;
        transform->setMatrix(osg::Matrixd::rotate(osg::Vec3d(0.0, 1.0, 0.0), osg::Vec3d(0.0, 0.0, 1.0)));

        for (unsigned int i = 0; i < model.scenes.size(); i++)
        {
            const tinygltf::Scene &scene = model.scenes[i];

            for (size_t j = 0; j < scene.nodes.size(); j++) {
                osg::Node* node = builder.createNode(model.nodes[scene.nodes[j]]);
                if (node)
                {
                    transform->addChild(node);
                }
            }
        }

        return transform;
    }

    struct NodeBuilder
    {
        const GLTFReader* reader;
        const tinygltf::Model &model;
        const Env& env;
        std::vector< osg::ref_ptr< osg::Array > > arrays;

        NodeBuilder(const GLTFReader* reader_, const tinygltf::Model &model_, const Env& env_)
            : reader(reader_), model(model_), env(env_)
        {
            extractArrays(arrays);
        }

        osg::Node* createNode(const tinygltf::Node& node) const
        {
            osg::MatrixTransform* mt = new osg::MatrixTransform;
            mt->setName(node.name);
            if (node.matrix.size() == 16)
            {
                osg::Matrixd mat;
                mat.set(node.matrix.data());
                mt->setMatrix(mat);
            }
            else
            {
                osg::Matrixd scale, translation, rotation;
                if (node.scale.size() == 3)
                {
                    scale = osg::Matrixd::scale(node.scale[0], node.scale[1], node.scale[2]);
                }

                if (node.rotation.size() == 4) {
                    osg::Quat quat(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
                    rotation.makeRotate(quat);
                }

                if (node.translation.size() == 3) {
                    translation = osg::Matrixd::translate(node.translation[0], node.translation[1], node.translation[2]);
                }

                mt->setMatrix(scale * rotation * translation);
            }


            // todo transformation
            if (node.mesh >= 0)
            {
                mt->addChild(makeMesh(model.meshes[node.mesh]));
            }

            // Load any children.
            for (unsigned int i = 0; i < node.children.size(); i++)
            {
                osg::Node* child = createNode(model.nodes[node.children[i]]);
                if (child)
                {
                    mt->addChild(child);
                }
            }
            return mt;
        }

        osg::Texture2D* makeTextureFromModel(const tinygltf::Texture& texture) const

        {
            const tinygltf::Image& image = model.images[texture.source];
            bool imageEmbedded =
                tinygltf::IsDataURI(image.uri) ||
                image.image.size() > 0;

            osgEarth::URI imageURI(image.uri, env.referrer);

            osg::ref_ptr<osg::Texture2D> tex;

            OE_DEBUG << "New Texture: " << imageURI.full() << ", embedded=" << imageEmbedded << std::endl;

            // First load the image
            osg::ref_ptr<osg::Image> img;

            if (image.image.size() > 0)
            {
                GLenum format = GL_RGB, texFormat = GL_RGB8;
                if (image.component == 4) format = GL_RGBA, texFormat = GL_RGBA8;

                img = new osg::Image();
                //OE_NOTICE << "Loading image of size " << image.width << "x" << image.height << " components = " << image.component << " totalSize=" << image.image.size() << std::endl;
                unsigned char *imgData = new unsigned char[image.image.size()];
                memcpy(imgData, &image.image[0], image.image.size());
                img->setImage(image.width, image.height, 1, texFormat, format, GL_UNSIGNED_BYTE, imgData, osg::Image::AllocationMode::USE_NEW_DELETE);
            }

            else if (!imageEmbedded) // load from URI
            {
                osgEarth::ReadResult rr = imageURI.readImage(env.readOptions);
                if(rr.succeeded())
                {
                    img = rr.releaseImage();
                    if (img.valid())
                    {
                        img->flipVertical();
                    }
                }
            }

            // If the image loaded OK, create the texture
            if (img.valid())
            {
                if(img->getPixelFormat() == GL_RGB)
                    img->setInternalTextureFormat(GL_RGB8);
                else if (img->getPixelFormat() == GL_RGBA)
                    img->setInternalTextureFormat(GL_RGBA8);

                tex = new osg::Texture2D(img.get());
                tex->setUnRefImageDataAfterApply(imageEmbedded);
                tex->setResizeNonPowerOfTwoHint(false);
                tex->setDataVariance(osg::Object::STATIC);

                if (texture.sampler >= 0 && texture.sampler < model.samplers.size())
                {
                    const tinygltf::Sampler& sampler = model.samplers[texture.sampler];
                    //tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)sampler.minFilter);
                    //tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)sampler.magFilter);
                    tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR); //sampler.minFilter);
                    tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR); //sampler.magFilter);
                    tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)sampler.wrapS);
                    tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)sampler.wrapT);
                    tex->setWrap(osg::Texture::WRAP_R, (osg::Texture::WrapMode)sampler.wrapR);
                }
                else
                {
                    tex->setFilter(osg::Texture::MIN_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR_MIPMAP_LINEAR);
                    tex->setFilter(osg::Texture::MAG_FILTER, (osg::Texture::FilterMode)osg::Texture::LINEAR);
                    tex->setWrap(osg::Texture::WRAP_S, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                    tex->setWrap(osg::Texture::WRAP_T, (osg::Texture::WrapMode)osg::Texture::CLAMP_TO_EDGE);
                }
            }
            return tex.release();
        }

        osg::Node* makeMesh(const tinygltf::Mesh& mesh) const
        {
            osg::Group *group = new osg::Group;

            OE_DEBUG << "Drawing " << mesh.primitives.size() << " primitives in mesh" << std::endl;

            for (size_t i = 0; i < mesh.primitives.size(); i++) {

                OE_DEBUG << " Processing primitive " << i << std::endl;
                const tinygltf::Primitive &primitive = mesh.primitives[i];
                if (primitive.indices < 0)
                {
                    // Hmm, should delete group here
                    return 0;
                }

                osg::ref_ptr< osg::Geometry > geom = new osg::Geometry;
                geom->setUseVertexBufferObjects(true);

                group->addChild(geom.get());

                // The base color factor of the material
                osg::Vec4 baseColorFactor(1.0f, 1.0f, 1.0f, 1.0f);

                if (primitive.material >= 0 && primitive.material < model.materials.size())
                {
                    const tinygltf::Material& material = model.materials[primitive.material];

                    /*
                      OSG_NOTICE << "extCommonValues=" << material.extCommonValues.size() << std::endl;
                      for (ParameterMap::iterator paramItr = material.extCommonValues.begin(); paramItr != material.extCommonValues.end(); ++paramItr)
                      {
                      OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                      }
                    */

                    OE_DEBUG << "additionalValues=" << material.additionalValues.size() << std::endl;
                    for (tinygltf::ParameterMap::const_iterator paramItr = material.additionalValues.begin(); paramItr != material.additionalValues.end(); ++paramItr)
                    {
                        OE_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                    }

                    //OSG_NOTICE << "values=" << material.values.size() << std::endl;
                    for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                    {
                        if (paramItr->first == "baseColorFactor")
                        {
                            tinygltf::ColorValue color = paramItr->second.ColorFactor();
                            baseColorFactor = osg::Vec4(color[0], color[1], color[2], color[3]);
                        }
                        else
                        {
                            OE_DEBUG << "    " << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                        }

                    }
                    /*
                      OSG_NOTICE << "extPBRValues=" << material.extPBRValues.size() << std::endl;
                      for (ParameterMap::iterator paramItr = material.extPBRValues.begin(); paramItr != material.extPBRValues.end(); ++paramItr)
                      {
                      OSG_NOTICE << paramItr->first << "=" << paramItr->second.string_value << std::endl;
                      }
                    */

                    for (tinygltf::ParameterMap::const_iterator paramItr = material.values.begin(); paramItr != material.values.end(); ++paramItr)
                    {
                        if (paramItr->first == "baseColorTexture")
                        {
                            std::map< std::string, double>::const_iterator i = paramItr->second.json_double_value.find("index");
                            if (i != paramItr->second.json_double_value.end())
                            {
                                int index = i->second;
                                const tinygltf::Texture& texture = model.textures[index];
                                const tinygltf::Image& image = model.images[texture.source];
                                // don't cache embedded textures!
                                bool imageEmbedded =
                                    tinygltf::IsDataURI(image.uri) ||
                                    image.image.size() > 0;
                                osgEarth::URI imageURI(image.uri, env.referrer);
                                osg::ref_ptr<osg::Texture2D> tex;
                                bool cachedTex = false;
                                TextureCache* texCache = reader->_texCache;
                                if (!imageEmbedded && texCache)
                                {
                                    OpenThreads::ScopedLock<TextureCache> lock(*texCache);
                                    auto texItr = texCache->find(imageURI.full());
                                    if (texItr != texCache->end())
                                    {
                                        tex = texItr->second;
                                        cachedTex = true;
                                    }
                                }

                                if (!tex.valid())
                                {
                                    tex = makeTextureFromModel(texture);
                                }

                                if (tex.valid())
                                {
                                    if (!imageEmbedded && texCache && !cachedTex)
                                    {
                                        OpenThreads::ScopedLock<TextureCache> lock(*texCache);
                                        auto insResult = texCache->insert(TextureCache::value_type(imageURI.full(), tex));
                                        if (insResult.second)
                                        {
                                            // Some other loader thread beat us in the cache
                                            tex = insResult.first->second;
                                        }
                                    }
                                    geom->getOrCreateStateSet()->setTextureAttributeAndModes(0, tex.get());
                                }

                                if (material.alphaMode != "OPAQUE")
                                {
                                    if (material.alphaMode == "BLEND")
                                    {
                                        geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                                        geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                                        osgEarth::Util::DiscardAlphaFragments().install(geom->getOrCreateStateSet(), 0.15);
                                    }
                                    else if (material.alphaMode == "MASK")
                                    {
                                        geom->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
                                        geom->getOrCreateStateSet()->setRenderingHint(osg::StateSet::TRANSPARENT_BIN);
                                        osgEarth::Util::DiscardAlphaFragments().install(geom->getOrCreateStateSet(), material.alphaCutoff);
                                    }
                                }
                            }
                        }
                    }
                }
            
                std::map<std::string, int>::const_iterator it(primitive.attributes.begin());
                std::map<std::string, int>::const_iterator itEnd(
                    primitive.attributes.end());

                for (; it != itEnd; it++)
                {
                    const tinygltf::Accessor &accessor = model.accessors[it->second];

                    if (it->first.compare("POSITION") == 0)
                    {
                        geom->setVertexArray(arrays[it->second].get());
                    }
                    else if (it->first.compare("NORMAL") == 0)
                    {
                        geom->setNormalArray(arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_0") == 0)
                    {
                        geom->setTexCoordArray(0, arrays[it->second].get());
                    }
                    else if (it->first.compare("TEXCOORD_1") == 0)
                    {
                        geom->setTexCoordArray(1, arrays[it->second].get());
                    }
                    else if (it->first.compare("COLOR_0") == 0)
                    {
                        // TODO:  Multipy by the baseColorFactor here?
                        OE_DEBUG << "Setting color array " << arrays[it->second].get() << std::endl;
                        geom->setColorArray(arrays[it->second].get());
                    }
                    else
                    {
                        OE_DEBUG << "Skipping array " << it->first << std::endl;
                    }
                }

                // If there is no color array just add one that has the base color factor in it.
                if (!geom->getColorArray())
                {
                    osg::Vec4Array* colors = new osg::Vec4Array();
                    osg::Vec3Array* verts = static_cast<osg::Vec3Array*>(geom->getVertexArray());
                    for (unsigned int i = 0; i < verts->size(); i++)
                    {
                        colors->push_back(baseColorFactor);
                    }
                    geom->setColorArray(colors, osg::Array::BIND_PER_VERTEX);
                }

                const tinygltf::Accessor &indexAccessor =
                    model.accessors[primitive.indices];

                int mode = -1;
                if (primitive.mode == TINYGLTF_MODE_TRIANGLES) {
                    mode = GL_TRIANGLES;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
                    mode = GL_TRIANGLE_STRIP;
                }
                else if (primitive.mode == TINYGLTF_MODE_TRIANGLE_FAN) {
                    mode = GL_TRIANGLE_FAN;
                }
                else if (primitive.mode == TINYGLTF_MODE_POINTS) {
                    mode = GL_POINTS;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE) {
                    mode = GL_LINES;
                }
                else if (primitive.mode == TINYGLTF_MODE_LINE_LOOP) {
                    mode = GL_LINE_LOOP;
                }

                {
                    const tinygltf::BufferView& bufferView = model.bufferViews[indexAccessor.bufferView];
                    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];

                    if (indexAccessor.componentType == GL_UNSIGNED_SHORT)
                    {
                        osg::DrawElementsUShort* drawElements = new osg::DrawElementsUShort(mode);
                        unsigned short* indices = (unsigned short*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                        drawElements->reserve(indexAccessor.count);
                        for (unsigned int j = 0; j < indexAccessor.count; j++)
                        {
                            unsigned short index = indices[j];
                            drawElements->push_back(index);
                        }
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == GL_UNSIGNED_INT)
                    {
                        osg::DrawElementsUInt* drawElements = new osg::DrawElementsUInt(mode);
                        drawElements->reserve(indexAccessor.count);
                        unsigned int* indices = (unsigned int*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                        for (unsigned int j = 0; j < indexAccessor.count; j++)
                        {
                            unsigned int index = indices[j];
                            drawElements->push_back(index);
                        }
                        geom->addPrimitiveSet(drawElements);
                    }
                    else if (indexAccessor.componentType == GL_UNSIGNED_BYTE)
                    {
                        osg::DrawElementsUByte* drawElements = new osg::DrawElementsUByte(mode);
                        drawElements->reserve(indexAccessor.count);
                        unsigned char* indices = (unsigned char*)(&buffer.data.at(0) + bufferView.byteOffset + indexAccessor.byteOffset);
                        for (unsigned int j = 0; j < indexAccessor.count; j++)
                        {
                            unsigned char index = indices[j];
                            drawElements->push_back(index);
                        }
                        geom->addPrimitiveSet(drawElements);
                    }
                }

                if (!env.readOptions || env.readOptions->getOptionString().find("gltfSkipNormals") == std::string::npos)
                {
                    // Generate normals automatically if we're not given any in the file itself.
                    if (!geom->getNormalArray())
                    {
                        osgUtil::SmoothingVisitor sv;
                        geom->accept(sv);
                    }
                }

                osgEarth::Registry::shaderGenerator().run(geom.get());
            }

            return group;
        }

        // Parameterize the creation of OSG arrays from glTF
        // accessors. It's a bit gratuitous to make ComponentType and
        // AccessorType template parameters. The thought was that the
        // memcpy could be optimized if these were constants in the
        // copyData() function, but that's debatable.

        template<typename OSGArray, int ComponentType, int AccessorType>
        class ArrayBuilder
        {
        public:
            static OSGArray* makeArray(unsigned int size)
            {
                return new OSGArray(size);
            }
            static void copyData(OSGArray* dest, const unsigned char* src, size_t viewOffset,
                                 size_t byteStride,  size_t accessorOffset, size_t count)
            {
                int32_t componentSize = tinygltf::GetComponentSizeInBytes(ComponentType);
                int32_t numComponents = tinygltf::GetNumComponentsInType(AccessorType);
                if (byteStride == 0)
                {
                    memcpy(&(*dest)[0], src + accessorOffset + viewOffset, componentSize * numComponents * count);
                }
                else
                {
                    const unsigned char* ptr = src + accessorOffset + viewOffset;
                    for (int i = 0; i < count; ++i, ptr += byteStride)
                    {
                        memcpy(&(*dest)[i], ptr, componentSize * numComponents);
                    }
                }
            }
            static void copyData(OSGArray* dest, const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                 const tinygltf::Accessor& accessor)
            {
                copyData(dest, &buffer.data.at(0), bufferView.byteOffset,
                         bufferView.byteStride, accessor.byteOffset, accessor.count);
            }
            static OSGArray* makeArray(const tinygltf::Buffer& buffer, const tinygltf::BufferView& bufferView,
                                       const tinygltf::Accessor& accessor)
            {
                OSGArray* result = new OSGArray(accessor.count);
                copyData(result, buffer, bufferView, accessor);
                return result;
            }
        };

        // Take all of the accessors and turn them into arrays
        void extractArrays(std::vector<osg::ref_ptr<osg::Array>> &arrays) const
        {
            for (unsigned int i = 0; i < model.accessors.size(); i++)
            {
                const tinygltf::Accessor& accessor = model.accessors[i];
                const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
                const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
                osg::ref_ptr< osg::Array > osgArray;

                switch (accessor.componentType)
                {
                case TINYGLTF_COMPONENT_TYPE_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ByteArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4bArray,
                                                TINYGLTF_COMPONENT_TYPE_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UByteArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4ubArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::ShortArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4sArray,
                                                TINYGLTF_COMPONENT_TYPE_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UShortArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4usArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::IntArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4uiArray,
                                                TINYGLTF_COMPONENT_TYPE_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::UIntArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4iArray,
                                                TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                    break;
                case TINYGLTF_COMPONENT_TYPE_FLOAT:
                    switch (accessor.type)
                    {
                    case TINYGLTF_TYPE_SCALAR:
                        osgArray = ArrayBuilder<osg::FloatArray,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_SCALAR>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC2:
                        osgArray = ArrayBuilder<osg::Vec2Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC2>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC3:
                        osgArray = ArrayBuilder<osg::Vec3Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC3>::makeArray(buffer, bufferView, accessor);
                        break;
                    case TINYGLTF_TYPE_VEC4:
                        osgArray = ArrayBuilder<osg::Vec4Array,
                                                TINYGLTF_COMPONENT_TYPE_FLOAT,
                                                TINYGLTF_TYPE_VEC4>::makeArray(buffer, bufferView, accessor);
                        break;
                    default:
                        break;
                    }
                default:
                    break;
                }
                if (osgArray.valid())
                {
                    osgArray->setBinding(osg::Array::BIND_PER_VERTEX);
                    osgArray->setNormalize(accessor.normalized);
                }
                else
                {
                    OSG_DEBUG << "Adding null array for " << i << std::endl;
                }
                arrays.push_back(osgArray);
            }
        }
    };

};

#endif // OSGEARTH_GLTF_READER_H
