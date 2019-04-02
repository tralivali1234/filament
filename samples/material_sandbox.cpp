/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <map>
#include <vector>

#include <getopt/getopt.h>

#include <imgui.h>
#include <filagui/ImGuiExtensions.h>

#include <utils/Path.h>

#include <filament/Engine.h>
#include <filament/DebugRegistry.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>
#include <filament/Material.h>
#include <filament/MaterialInstance.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <filament/View.h>

#include <math/mat3.h>
#include <math/mat4.h>
#include <math/vec4.h>
#include <math/norm.h>

#include "app/Config.h"
#include "app/IBL.h"
#include "app/FilamentApp.h"
#include "app/MeshAssimp.h"

#include "material_sandbox.h"

using namespace filament::math;
using namespace filament;
using namespace filamat;
using namespace utils;

static std::vector<Path> g_filenames;

static Scene* g_scene = nullptr;

std::unique_ptr<MeshAssimp> g_meshSet;
static std::map<std::string, MaterialInstance*> g_meshMaterialInstances;
static SandboxParameters g_params;
static Config g_config;
static bool g_shadowPlane = false;

static void printUsage(char* name) {
    std::string exec_name(Path(name).getName());
    std::string usage(
            "SAMPLE_MATERIAL showcases all material models\n"
            "Usage:\n"
            "    SAMPLE_MATERIAL [options] <mesh files (.obj, .fbx, COLLADA)>\n"
            "Options:\n"
            "   --help, -h\n"
            "       Prints this message\n\n"
            "   --api, -a\n"
            "       Specify the backend API: opengl (default), vulkan, or metal\n\n"
            "   --ibl=<path to cmgen IBL>, -i <path>\n"
            "       Applies an IBL generated by cmgen's deploy option\n\n"
            "   --split-view, -v\n"
            "       Splits the window into 4 views\n\n"
            "   --scale=[number], -s [number]\n"
            "       Applies uniform scale\n\n"
            "   --shadow-plane, -p\n"
            "       Enable shadow plane\n\n"
    );
    const std::string from("SAMPLE_MATERIAL");
    for (size_t pos = usage.find(from); pos != std::string::npos; pos = usage.find(from, pos)) {
        usage.replace(pos, from.length(), exec_name);
    }
    std::cout << usage;
}

static int handleCommandLineArgments(int argc, char* argv[], Config* config) {
    static constexpr const char* OPTSTR = "ha:vps:i:";
    static const struct option OPTIONS[] = {
            { "help",       no_argument,       nullptr, 'h' },
            { "api",        required_argument, nullptr, 'a' },
            { "ibl",        required_argument, nullptr, 'i' },
            { "split-view", no_argument,       nullptr, 'v' },
            { "scale",      required_argument, nullptr, 's' },
            { "shadow-plane", no_argument,     nullptr, 'p' },
            { nullptr, 0, nullptr, 0 }  // termination of the option list
    };
    int opt;
    int option_index = 0;
    while ((opt = getopt_long(argc, argv, OPTSTR, OPTIONS, &option_index)) >= 0) {
        std::string arg(optarg ? optarg : "");
        switch (opt) {
            default:
            case 'h':
                printUsage(argv[0]);
                exit(0);
            case 'a':
                if (arg == "opengl") {
                    config->backend = Engine::Backend::OPENGL;
                } else if (arg == "vulkan") {
                    config->backend = Engine::Backend::VULKAN;
                } else if (arg == "metal") {
                    config->backend = Engine::Backend::METAL;
                } else {
                    std::cerr << "Unrecognized backend. Must be 'opengl'|'vulkan'|'metal'." << std::endl;
                }
                break;
            case 'i':
                config->iblDirectory = arg;
                break;
            case 's':
                try {
                    config->scale = std::stof(arg);
                } catch (std::invalid_argument& e) {
                    // keep scale of 1.0
                } catch (std::out_of_range& e) {
                    // keep scale of 1.0
                }
                break;
            case 'v':
                config->splitView = true;
                break;
            case 'p':
                g_shadowPlane = true;
                break;
        }
    }

    return optind;
}

static void cleanup(Engine* engine, View*, Scene*) {
    for (const auto& material : g_meshMaterialInstances) {
        engine->destroy(material.second);
    }

    for (auto& i : g_params.materialInstance) {
        engine->destroy(i);
    }

    for (auto& i : g_params.material) {
        engine->destroy(i);
    }

    g_meshSet.reset(nullptr);

    engine->destroy(g_params.light);
    EntityManager& em = EntityManager::get();
    em.destroy(g_params.light);
}

static void setup(Engine* engine, View*, Scene* scene) {
    g_scene = scene;

    g_meshSet = std::make_unique<MeshAssimp>(*engine);

    createInstances(g_params, *engine);

    for (auto& filename : g_filenames) {
        g_meshSet->addFromFile(filename, g_meshMaterialInstances);
    }

    auto& tcm = engine->getTransformManager();
    auto ei = tcm.getInstance(g_meshSet->getRenderables()[0]);
    tcm.setTransform(ei, mat4f{ mat3f(g_config.scale), float3(0.0f, 0.0f, -4.0f) } *
            tcm.getWorldTransform(ei));

    auto& rcm = engine->getRenderableManager();
    for (auto renderable : g_meshSet->getRenderables()) {
        auto instance = rcm.getInstance(renderable);
        if (!instance) continue;

        rcm.setCastShadows(instance, g_params.castShadows);

        for (size_t i = 0; i < rcm.getPrimitiveCount(instance); i++) {
            rcm.setMaterialInstanceAt(instance, i, g_params.materialInstance[MATERIAL_LIT]);
        }

        scene->addEntity(renderable);
    }

    scene->addEntity(g_params.light);

    if (g_shadowPlane) {
        EntityManager& em = EntityManager::get();
        Material* shadowMaterial = Material::Builder()
                .package(RESOURCES_GROUNDSHADOW_DATA, RESOURCES_GROUNDSHADOW_SIZE)
                .build(*engine);

        const static uint32_t indices[] = {
                0, 1, 2, 2, 3, 0
        };

        const static filament::math::float3 vertices[] = {
                { -10, 0, -10 },
                { -10, 0,  10 },
                {  10, 0,  10 },
                {  10, 0, -10 },
        };

        short4 tbn = filament::math::packSnorm16(
                mat3f::packTangentFrame(
                        filament::math::mat3f{
                                float3{ 1.0f, 0.0f, 0.0f }, float3{ 0.0f, 0.0f, 1.0f },
                                float3{ 0.0f, 1.0f, 0.0f }
                        }
                ).xyzw);

        const static filament::math::short4 normals[] { tbn, tbn, tbn, tbn };

        VertexBuffer* vertexBuffer = VertexBuffer::Builder()
                .vertexCount(4)
                .bufferCount(2)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT3)
                .attribute(VertexAttribute::TANGENTS, 1, VertexBuffer::AttributeType::SHORT4)
                .normalized(VertexAttribute::TANGENTS)
                .build(*engine);

        vertexBuffer->setBufferAt(*engine, 0, VertexBuffer::BufferDescriptor(
                vertices, vertexBuffer->getVertexCount() * sizeof(vertices[0])));
        vertexBuffer->setBufferAt(*engine, 1, VertexBuffer::BufferDescriptor(
                normals, vertexBuffer->getVertexCount() * sizeof(normals[0])));

        IndexBuffer* indexBuffer = IndexBuffer::Builder()
                .indexCount(6)
                .build(*engine);

        indexBuffer->setBuffer(*engine, IndexBuffer::BufferDescriptor(
                indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

        Entity planeRenderable = em.create();
        RenderableManager::Builder(1)
                .boundingBox({{ 0, 0, 0 },
                              { 10, 1e-4f, 10 }})
                .material(0, shadowMaterial->getDefaultInstance())
                .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                        vertexBuffer, indexBuffer, 0, 6)
                .culling(false)
                .receiveShadows(true)
                .castShadows(false)
                .build(*engine, planeRenderable);

        scene->addEntity(planeRenderable);

        tcm.setTransform(tcm.getInstance(planeRenderable),
                filament::math::mat4f::translation(float3{ 0, -1, -4 }));
    }
}

static void gui(filament::Engine* engine, filament::View*) {
    auto& params = g_params;
    ImGui::SetNextWindowSize(ImVec2(0.0f, 0.0f));
    ImGui::Begin("Parameters");
    {
        if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Combo("model", &params.currentMaterialModel,
                    "unlit\0lit\0subsurface\0cloth\0\0");

            if (params.currentMaterialModel == MATERIAL_MODEL_LIT) {
                ImGui::Combo("blending", &params.currentBlending,
                        "opaque\0transparent\0fade\0\0");
            }

            ImGui::ColorEdit3("baseColor", &params.color.r);

            if (params.currentMaterialModel > MATERIAL_MODEL_UNLIT) {
                if (params.currentBlending == BLENDING_TRANSPARENT ||
                        params.currentBlending == BLENDING_FADE) {
                    ImGui::SliderFloat("alpha", &params.alpha, 0.0f, 1.0f);
                }
                ImGui::SliderFloat("roughness", &params.roughness, 0.0f, 1.0f);
                if (params.currentMaterialModel != MATERIAL_MODEL_CLOTH) {
                    ImGui::SliderFloat("metallic", &params.metallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("reflectance", &params.reflectance, 0.0f, 1.0f);
                }
                if (params.currentMaterialModel != MATERIAL_MODEL_CLOTH &&
                        params.currentMaterialModel != MATERIAL_MODEL_SUBSURFACE) {
                    ImGui::SliderFloat("clearCoat", &params.clearCoat, 0.0f, 1.0f);
                    ImGui::SliderFloat("clearCoatRoughness", &params.clearCoatRoughness, 0.0f, 1.0f);
                    ImGui::SliderFloat("anisotropy", &params.anisotropy, -1.0f, 1.0f);
                }
                if (params.currentMaterialModel == MATERIAL_MODEL_SUBSURFACE) {
                    ImGui::SliderFloat("thickness", &params.thickness, 0.0f, 1.0f);
                    ImGui::SliderFloat("subsurfacePower", &params.subsurfacePower, 1.0f, 24.0f);
                    ImGui::ColorEdit3("subsurfaceColor", &params.subsurfaceColor.r);
                }
                if (params.currentMaterialModel == MATERIAL_MODEL_CLOTH) {
                    ImGui::ColorEdit3("sheenColor", &params.sheenColor.r);
                    ImGui::ColorEdit3("subsurfaceColor", &params.subsurfaceColor.r);
                }
            }
        }

        if (ImGui::CollapsingHeader("Object")) {
            ImGui::Checkbox("castShadows", &params.castShadows);
        }

        if (ImGui::CollapsingHeader("Light")) {
            ImGui::Checkbox("enabled", &params.directionalLightEnabled);
            ImGui::ColorEdit3("color", &params.lightColor.r);
            ImGui::SliderFloat("lux", &params.lightIntensity, 0.0f, 150000.0f);
            ImGui::SliderFloat("sunSize", &params.sunAngularRadius, 0.1f, 10.0f);
            ImGui::SliderFloat("haloSize", &params.sunHaloSize, 1.01f, 40.0f);
            ImGui::SliderFloat("haloFalloff", &params.sunHaloFalloff, 0.0f, 2048.0f);
            ImGui::SliderFloat("ibl", &params.iblIntensity, 0.0f, 50000.0f);
            ImGui::SliderAngle("ibl rotation", &params.iblRotation);
            ImGuiExt::DirectionWidget("direction", &params.lightDirection.x);
        }

        if (ImGui::CollapsingHeader("Post-processing")) {
            ImGui::Checkbox("msaa 4x", &params.msaa);
            ImGui::Checkbox("tone-mapping", &params.tonemapping);
            ImGui::Indent();
                ImGui::Checkbox("dithering", &params.dithering);
                ImGui::Unindent();
            ImGui::Checkbox("fxaa", &params.fxaa);
        }

        if (ImGui::CollapsingHeader("Debug")) {
            DebugRegistry& debug = engine->getDebugRegistry();
            ImGui::Checkbox("Camera at origin",
                    debug.getPropertyAddress<bool>("d.view.camera_at_origin"));
            ImGui::Checkbox("Light Far uses shadow casters",
                    debug.getPropertyAddress<bool>("d.shadowmap.far_uses_shadowcasters"));
            ImGui::Checkbox("Focus shadow casters",
                    debug.getPropertyAddress<bool>("d.shadowmap.focus_shadowcasters"));
            bool* lispsm;
            if (debug.getPropertyAddress<bool>("d.shadowmap.lispsm", &lispsm)) {
                ImGui::Checkbox("Enable LiSPSM", lispsm);
                if (*lispsm) {
                    ImGui::SliderFloat("dzn",
                            debug.getPropertyAddress<float>("d.shadowmap.dzn"), 0.0f, 1.0f);
                    ImGui::SliderFloat("dzf",
                            debug.getPropertyAddress<float>("d.shadowmap.dzf"),-1.0f, 0.0f);
                }
            }
        }
    }
    ImGui::End();

    MaterialInstance* materialInstance = updateInstances(params, *engine);

    auto& rcm = engine->getRenderableManager();
    for (auto renderable : g_meshSet->getRenderables()) {
        auto instance = rcm.getInstance(renderable);
        if (!instance) continue;
        for (size_t i = 0; i < rcm.getPrimitiveCount(instance); i++) {
            rcm.setMaterialInstanceAt(instance, i, materialInstance);
        }
        rcm.setCastShadows(instance, params.castShadows);
    }

    if (params.directionalLightEnabled && !params.hasDirectionalLight) {
        g_scene->addEntity(params.light);
        params.hasDirectionalLight = true;
    } else if (!params.directionalLightEnabled && params.hasDirectionalLight) {
        g_scene->remove(params.light);
        params.hasDirectionalLight = false;
    }

    auto* ibl = FilamentApp::get().getIBL();
    if (ibl) {
        ibl->getIndirectLight()->setIntensity(params.iblIntensity);
        ibl->getIndirectLight()->setRotation(
                mat3f::rotation(params.iblRotation, float3{ 0, 1, 0 }));
    }
}

static void preRender(filament::Engine*, filament::View* view, filament::Scene*, filament::Renderer*) {
    view->setAntiAliasing(g_params.fxaa ? View::AntiAliasing::FXAA : View::AntiAliasing::NONE);
    view->setToneMapping(g_params.tonemapping ? View::ToneMapping::ACES : View::ToneMapping::LINEAR);
    view->setDithering(g_params.dithering ? View::Dithering::TEMPORAL : View::Dithering::NONE);
    view->setSampleCount((uint8_t) (g_params.msaa ? 4 : 1));
}

int main(int argc, char* argv[]) {
    int option_index = handleCommandLineArgments(argc, argv, &g_config);
    int num_args = argc - option_index;
    if (num_args < 1) {
        printUsage(argv[0]);
        return 1;
    }

    for (int i = option_index; i < argc; i++) {
        utils::Path filename = argv[i];
        if (!filename.exists()) {
            std::cerr << "file " << argv[i] << " not found!" << std::endl;
            return 1;
        }
        g_filenames.push_back(filename);
    }

    g_config.title = "Material Sandbox";
    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.run(g_config, setup, cleanup, gui, preRender);

    return 0;
}
