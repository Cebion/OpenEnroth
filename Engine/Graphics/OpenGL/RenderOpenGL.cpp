#ifdef _WINDOWS
    #pragma comment(lib, "opengl32.lib")
    #pragma comment(lib, "glu32.lib")

    //  on windows, this is required in gl/glu.h
    #if !defined(APIENTRY)
        #define APIENTRY __stdcall
    #endif

    #if !defined(WINGDIAPI)
        #define WINGDIAPI
    #endif

    #if !defined(CALLBACK)
        #define CALLBACK __stdcall
    #endif
#endif

#include "glad/gl.h"

#ifdef __APPLE__
#include <OpenGL/glu.h>
#else
#include <GL/glu.h>
#endif

#include <algorithm>
#include <memory>
#include <utility>
#include <map>

#include <glm.hpp>
#include <glm/glm/gtc/matrix_transform.hpp>

#include "Engine/Engine.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/ImageLoader.h"
#include "Engine/Graphics/LightmapBuilder.h"
#include "Engine/Graphics/DecalBuilder.h"
#include "Engine/Graphics/Level/Decoration.h"
#include "Engine/Graphics/DecorationList.h"
#include "Engine/Graphics/Lights.h"
#include "Engine/Graphics/Nuklear.h"
#include "Engine/Graphics/OpenGL/RenderOpenGL.h"
#include "Engine/Graphics/OpenGL/TextureOpenGL.h"
#include "Engine/Graphics/OpenGL/GLShaderLoader.h"
#include "Engine/Graphics/Outdoor.h"
#include "Engine/Graphics/ParticleEngine.h"
#include "Engine/Graphics/PCX.h"
#include "Engine/Graphics/Sprites.h"
#include "Engine/Graphics/Viewport.h"
#include "Engine/Graphics/Weather.h"
#include "Engine/Objects/Actor.h"
#include "Engine/Objects/ObjectList.h"
#include "Engine/Objects/SpriteObject.h"
#include "Engine/OurMath.h"
#include "Engine/Party.h"
#include "Engine/SpellFxRenderer.h"
#include "Arcomage/Arcomage.h"

#include "Platform/Api.h"
#include "Platform/OSWindow.h"

#ifndef LOWORD
    #define LOWORD(l) ((unsigned short)(((std::uintptr_t)(l)) & 0xFFFF))
#endif

void _set_3d_projection_matrix();
void _set_3d_modelview_matrix();
void _set_ortho_projection(bool gameviewport = false);
void _set_ortho_modelview();

// improved error check
void GL_Check_Errors(bool breakonerr = true) {
    GLenum err = glGetError();

    while (err != GL_NO_ERROR) {
        static std::string error;

        switch (err) {
            case GL_INVALID_OPERATION:      error = "INVALID_OPERATION";      break;
            case GL_INVALID_ENUM:           error = "INVALID_ENUM";           break;
            case GL_INVALID_VALUE:          error = "INVALID_VALUE";          break;
            case GL_OUT_OF_MEMORY:          error = "OUT_OF_MEMORY";          break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:  error = "INVALID_FRAMEBUFFER_OPERATION";  break;
            default:                        error = "Unknown Error";  break;
        }

        logger->Warning("OpenGL error (%u): %s", err, error.c_str());
        if (breakonerr) __debugbreak();

        err = glGetError();
    }
}

// TODO(pskelton): move to opengl render class
// these are the view and projection matrices for submission to shaders
glm::mat4 projmat;
glm::mat4 viewmat;

RenderVertexSoft VertexRenderList[50];  // array_50AC10
RenderVertexD3D3 d3d_vertex_buffer[50];

void Polygon::_normalize_v_18() {
    float len = sqrt((double)this->v_18.z * (double)this->v_18.z +
                     (double)this->v_18.y * (double)this->v_18.y +
                     (double)this->v_18.x * (double)this->v_18.x);
    if (fabsf(len) < 1e-6f) {
        v_18.x = 0;
        v_18.y = 0;
        v_18.z = 65536;
    } else {
        v_18.x = round_to_int((double)this->v_18.x / len * 65536.0);
        v_18.y = round_to_int((double)this->v_18.y / len * 65536.0);
        v_18.z = round_to_int((double)this->v_18.z / len * 65536.0);
    }
}

bool IsBModelVisible(BSPModel *model, int reachable_depth, bool *reachable) {
    // checks if model is visible in FOV cone
    float halfangle = (pCamera3D->odm_fov_rad) / 2.0;
    float rayx = model->vBoundingCenter.x - pCamera3D->vCameraPos.x;
    float rayy = model->vBoundingCenter.y - pCamera3D->vCameraPos.y;

    // approx distance
    int dist = int_get_vector_length(abs(rayx), abs(rayy), 0);
    *reachable = false;
    if (dist < model->sBoundingRadius + reachable_depth) *reachable = true;

    // dot product of camvec and ray - size in forward
    float frontvec = rayx * pCamera3D->fRotationZCosine + rayy * pCamera3D->fRotationZSine;
    if (pCamera3D->sRotationY) { frontvec *= pCamera3D->fRotationYCosine;}

    // dot product of camvec and ray - size in left
    float leftvec = rayy * pCamera3D->fRotationZCosine - rayx * pCamera3D->fRotationZSine;

    // which half fov is ray in direction of - compare slopes
    float sloperem = 0.0;
    if (leftvec >= 0) {  // acute - left
        sloperem = frontvec * sin(halfangle) - leftvec * cos(halfangle);
    } else {  // obtuse - right
        sloperem = frontvec * sin(halfangle) + leftvec * cos(halfangle);
    }

    // view range check
    if (dist <= pCamera3D->GetFarClip() + 2048) {
        // boudning point inside cone
        if (sloperem >= 0) return true;
        // bounding radius inside cone
        if (abs(sloperem) < model->sBoundingRadius + 512) return true;
    }

    // not visible
    return false;
}

int GetActorTintColor(int max_dimm, int min_dimm, float distance, int a4, RenderBillboard *a5) {
    signed int v6;   // edx@1
    int v8;          // eax@3
    double v9;       // st7@12
    int v11;         // ecx@28
    double v15;      // st7@44
    int v18;         // ST14_4@44
    signed int v20;  // [sp+10h] [bp-4h]@10
    float a3c;       // [sp+1Ch] [bp+8h]@44
    int a5a;         // [sp+24h] [bp+10h]@44

    // v5 = a2;
    v6 = 0;

    if (uCurrentlyLoadedLevelType == LEVEL_Indoor)
        return 8 * (31 - max_dimm) | ((8 * (31 - max_dimm) | ((31 - max_dimm) << 11)) << 8);

    if (pParty->armageddon_timer) return 0xFFFF0000;

    v8 = pWeather->bNight;
    if (engine->IsUnderwater())
        v8 = 0;
    if (v8) {
        v20 = 1;
        if (pParty->pPartyBuffs[PARTY_BUFF_TORCHLIGHT].Active())
            v20 = pParty->pPartyBuffs[PARTY_BUFF_TORCHLIGHT].uPower;
        v9 = (double)v20 * 1024.0;
        if (a4) {
            v6 = 216;
            goto LABEL_20;
        }
        if (distance <= v9) {
            if (distance > 0.0) {
                // a4b = distance * 216.0 / device_caps;
                // v10 = a4b + 6.7553994e15;
                // v6 = LODWORD(v10);
                v6 = floorf(0.5f + distance * 216.0 / v9);
                if (v6 > 216) {
                    v6 = 216;
                    goto LABEL_20;
                }
            }
        } else {
            v6 = 216;
        }
        if (distance != 0.0) {
        LABEL_20:
            if (a5) v6 = 8 * _43F55F_get_billboard_light_level(a5, v6 >> 3);
            if (v6 > 216) v6 = 216;
            return (255 - v6) | ((255 - v6) << 16) | ((255 - v6) << 8);
        }
        // LABEL_19:
        v6 = 216;
        goto LABEL_20;
    }

    if (fabsf(distance) < 1.0e-6f) return 0xFFF8F8F8;

    // dim in measured in 8-steps
    v11 = 8 * (max_dimm - min_dimm);
    // v12 = v11;
    if (v11 >= 0) {
        if (v11 > 216) v11 = 216;
    } else {
        v11 = 0;
    }

    float fog_density_mult = 216.0f;
    if (a4)
        fog_density_mult +=
            distance / (double)pODMRenderParams->shading_dist_shade * 32.0;

    v6 = v11 + floorf(pOutdoor->fFogDensity * fog_density_mult + 0.5f);

    if (a5) v6 = 8 * _43F55F_get_billboard_light_level(a5, v6 >> 3);
    if (v6 > 216) v6 = 216;
    if (v6 < v11) v6 = v11;
    if (v6 > 8 * pOutdoor->max_terrain_dimming_level)
        v6 = 8 * pOutdoor->max_terrain_dimming_level;
    if (!engine->IsUnderwater()) {
        return (255 - v6) | ((255 - v6) << 16) | ((255 - v6) << 8);
    } else {
        v15 = (double)(255 - v6) * 0.0039215689;
        a3c = v15;
        // a4c = v15 * 16.0;
        // v16 = a4c + 6.7553994e15;
        a5a = floorf(v15 * 16.0 + 0.5f);  // LODWORD(v16);
                                          // a4d = a3c * 194.0;
                                          // v17 = a4d + 6.7553994e15;
        v18 = floorf(a3c * 194.0 + 0.5f);  // LODWORD(v17);
                                           // a3d = a3c * 153.0;
                                           // v19 = a3d + 6.7553994e15;
        return (int)floorf(a3c * 153.0 + 0.5f) /*LODWORD(v19)*/ |
               ((v18 | (a5a << 8)) << 8);
    }
}

std::shared_ptr<IRender> render;
int uNumDecorationsDrawnThisFrame;
RenderBillboard pBillboardRenderList[500];
unsigned int uNumBillboardsToDraw;
int uNumSpritesDrawnThisFrame;

RenderVertexSoft array_73D150[20];

void RenderOpenGL::MaskGameViewport() {
    // do not want in opengl mode
}

// ----- (0043F5C8) --------------------------------------------------------
int GetLightLevelAtPoint(unsigned int uBaseLightLevel, int uSectorID, float x, float y, float z) {
    int lightlevel = uBaseLightLevel;
    float light_radius{};
    float distX{};
    float distY{};
    float distZ{};
    unsigned int approx_distance;

    // mobile lights
    for (uint i = 0; i < pMobileLightsStack->uNumLightsActive; ++i) {
        MobileLight *p = &pMobileLightsStack->pLights[i];
        light_radius = p->uRadius;

        distX = abs(p->vPosition.x - x);
        if (distX <= light_radius) {
            distY = abs(p->vPosition.y - y);
            if (distY <= light_radius) {
                distZ = abs(p->vPosition.z - z);
                if (distZ <= light_radius) {
                    approx_distance = int_get_vector_length(distX, distY, distZ);
                    if (approx_distance < light_radius)
         //* ORIGONAL */lightlevel += ((unsigned __int64)(30i64 *(signed int)(approx_distance << 16) / light_radius) >> 16) - 30;
                        lightlevel += static_cast<int> (30 * approx_distance / light_radius) - 30;
                }
            }
        }
    }

    // sector lights
    if (uCurrentlyLoadedLevelType == LEVEL_Indoor) {
        BLVSector *pSector = &pIndoor->pSectors[uSectorID];

        for (uint i = 0; i < pSector->uNumLights; ++i) {
            BLVLightMM7 *this_light = pIndoor->pLights + pSector->pLights[i];
            light_radius = this_light->uRadius;

            if (~this_light->uAtributes & 8) {
                distX = abs(this_light->vPosition.x - x);
                if (distX <= light_radius) {
                    distY = abs(this_light->vPosition.y - y);
                    if (distY <= light_radius) {
                        distZ = abs(this_light->vPosition.z - z);
                        if (distZ <= light_radius) {
                            approx_distance = int_get_vector_length(distX, distY, distZ);
                            if (approx_distance < light_radius)
                                lightlevel += static_cast<int> (30 * approx_distance / light_radius) - 30;
                        }
                    }
                }
            }
        }
    }

    // stationary lights
    for (uint i = 0; i < pStationaryLightsStack->uNumLightsActive; ++i) {
        StationaryLight* p = &pStationaryLightsStack->pLights[i];
        light_radius = p->uRadius;

        distX = abs(p->vPosition.x - x);
        if (distX <= light_radius) {
            distY = abs(p->vPosition.y - y);
            if (distY <= light_radius) {
                distZ = abs(p->vPosition.z - z);
                if (distZ <= light_radius) {
                    approx_distance = int_get_vector_length(distX, distY, distZ);
                    if (approx_distance < light_radius)
                        lightlevel += static_cast<int> (30 * approx_distance / light_radius) - 30;
                }
            }
        }
    }

    lightlevel = std::clamp(lightlevel, 0, 31);
    return lightlevel;
}

void UpdateObjects() {
    int v5;   // ecx@6
    int v7;   // eax@9
    int v11;  // eax@17
    int v12;  // edi@27
    int v18;  // [sp+4h] [bp-10h]@27
    int v19;  // [sp+8h] [bp-Ch]@27

    for (uint i = 0; i < uNumSpriteObjects; ++i) {
        if (pSpriteObjects[i].uAttributes & OBJECT_40) {
            pSpriteObjects[i].uAttributes &= ~OBJECT_40;
        } else {
            ObjectDesc *object =
                &pObjectList->pObjects[pSpriteObjects[i].uObjectDescID];
            if (pSpriteObjects[i].AttachedToActor()) {
                v5 = PID_ID(pSpriteObjects[i].spell_target_pid);
                pSpriteObjects[i].vPosition.x = pActors[v5].vPosition.x;
                pSpriteObjects[i].vPosition.y = pActors[v5].vPosition.y;
                pSpriteObjects[i].vPosition.z =
                    pActors[v5].vPosition.z + pActors[v5].uActorHeight;
                if (!pSpriteObjects[i].uObjectDescID) continue;
                pSpriteObjects[i].uSpriteFrameID += pEventTimer->uTimeElapsed;
                if (!(object->uFlags & OBJECT_DESC_TEMPORARY)) continue;
                if (pSpriteObjects[i].uSpriteFrameID >= 0) {
                    v7 = object->uLifetime;
                    if (pSpriteObjects[i].uAttributes & ITEM_BROKEN)
                        v7 = pSpriteObjects[i].field_20;
                    if (pSpriteObjects[i].uSpriteFrameID < v7) continue;
                }
                SpriteObject::OnInteraction(i);
                continue;
            }
            if (pSpriteObjects[i].uObjectDescID) {
                pSpriteObjects[i].uSpriteFrameID += pEventTimer->uTimeElapsed;
                if (object->uFlags & OBJECT_DESC_TEMPORARY) {
                    if (pSpriteObjects[i].uSpriteFrameID < 0) {
                        SpriteObject::OnInteraction(i);
                        continue;
                    }
                    v11 = object->uLifetime;
                    if (pSpriteObjects[i].uAttributes & ITEM_BROKEN)
                        v11 = pSpriteObjects[i].field_20;
                }
                if (!(object->uFlags & OBJECT_DESC_TEMPORARY) ||
                    pSpriteObjects[i].uSpriteFrameID < v11) {
                    if (uCurrentlyLoadedLevelType == LEVEL_Indoor)
                        SpriteObject::UpdateObject_fn0_BLV(i);
                    else
                        SpriteObject::UpdateObject_fn0_ODM(i);
                    if (!pParty->bTurnBasedModeOn || !(pSpriteObjects[i].uSectorID & 4)) {
                        continue;
                    }
                    v12 = abs(pParty->vPosition.x -
                              pSpriteObjects[i].vPosition.x);
                    v18 = abs(pParty->vPosition.y -
                              pSpriteObjects[i].vPosition.y);
                    v19 = abs(pParty->vPosition.z -
                              pSpriteObjects[i].vPosition.z);
                    if (int_get_vector_length(v12, v18, v19) <= 5120) continue;
                    SpriteObject::OnInteraction(i);
                    continue;
                }
                if (!(object->uFlags & OBJECT_DESC_INTERACTABLE)) {
                    SpriteObject::OnInteraction(i);
                    continue;
                }
                _46BFFA_update_spell_fx(i, PID(OBJECT_Item, i));
            }
        }
    }
}

int _43F55F_get_billboard_light_level(RenderBillboard *a1,
                                      int uBaseLightLevel) {
    int v3 = 0;

    if (uCurrentlyLoadedLevelType == LEVEL_Indoor) {
        v3 = pIndoor->pSectors[a1->uIndoorSectorID].uMinAmbientLightLevel;
    } else {
        if (uBaseLightLevel == -1) {
            v3 = a1->dimming_level;
        } else {
            v3 = uBaseLightLevel;
        }
    }

    return GetLightLevelAtPoint(
        v3, a1->uIndoorSectorID, a1->world_x, a1->world_y, a1->world_z);
}

unsigned int sub_46DEF2(signed int a2, unsigned int uLayingItemID) {
    unsigned int result = uLayingItemID;
    if (pObjectList->pObjects[pSpriteObjects[uLayingItemID].uObjectDescID].uFlags & 0x10) {
        result = _46BFFA_update_spell_fx(uLayingItemID, a2);
    }
    return result;
}

RenderVertexSoft array_507D30[50];

// sky billboard stuff

void SkyBillboardStruct::CalcSkyFrustumVec(int x1, int y1, int z1, int x2, int y2, int z2) {
    // 6 0 0 0 6 0

    // TODO(pskelton): clean up

    float cosz = pCamera3D->fRotationZCosine;  // int_cosine_Z;
    float cosx = pCamera3D->fRotationYCosine;  // int_cosine_y;
    float sinz = pCamera3D->fRotationZSine;  // int_sine_Z;
    float sinx = pCamera3D->fRotationYSine;  // int_sine_y;

    // positions all minus ?
    float v11 = cosz * -pCamera3D->vCameraPos.x + sinz * -pCamera3D->vCameraPos.y;
    float v24 = cosz * -pCamera3D->vCameraPos.y - sinz * -pCamera3D->vCameraPos.x;

    // cam position transform
    if (pCamera3D->sRotationY) {
        this->field_0_party_dir_x = (v11 * cosx) + (-pCamera3D->vCameraPos.z * sinx);
        this->field_4_party_dir_y = v24;
        this->field_8_party_dir_z = (-pCamera3D->vCameraPos.z * cosx) /*-*/ + (v11 * sinx);
    } else {
        this->field_0_party_dir_x = v11;
        this->field_4_party_dir_y = v24;
        this->field_8_party_dir_z = (-pCamera3D->vCameraPos.z);
    }

    // set 1 position transfrom (6 0 0) looks like cam left vector
    if (pCamera3D->sRotationY) {
        float v17 = (x1 * cosz) + (y1 * sinz);

        this->CamVecLeft_Y = (v17 * cosx) + (z1 * sinx);  // dz
        this->CamVecLeft_X = (y1 * cosz) - (x1 * sinz);  // dx
        this->CamVecLeft_Z = (z1 * cosx) /*-*/ + (v17 * sinx);  // dy
    } else {
        this->CamVecLeft_Y = (x1 * cosz) + (y1 * sinz);  // dz
        this->CamVecLeft_X = (y1 * cosz) - (x1 * sinz);  // dx
        this->CamVecLeft_Z = z1;  // dy
    }

    // set 2 position transfrom (0 6 0) looks like cam front vector
    if (pCamera3D->sRotationY) {
        float v19 = (x2 * cosz) + (y2 * sinz);

        this->CamVecFront_Y = (v19 * cosx) + (z2 * sinx);  // dz
        this->CamVecFront_X = (y2 * cosz) - (x2 * sinz);  // dx
        this->CamVecFront_Z = (z2 * cosx) /*-*/ + (v19 * sinx);  // dy
    } else {
        this->CamVecFront_Y = (x2 * cosz) + (y2 * sinz);  // dz
        this->CamVecFront_X = (y2 * cosz) - (x2 * sinz);  // dx
        this->CamVecFront_Z = z2;  // dy
    }

    this->CamLeftDot =
        (this->CamVecLeft_X * this->field_0_party_dir_x) +
        (this->CamVecLeft_Y * this->field_4_party_dir_y) +
        (this->CamVecLeft_Z * this->field_8_party_dir_z);
    this->CamFrontDot =
        (this->CamVecFront_X * this->field_0_party_dir_x) +
        (this->CamVecFront_Y * this->field_4_party_dir_y) +
        (this->CamVecFront_Z * this->field_8_party_dir_z);
}

RenderOpenGL::RenderOpenGL(
    std::shared_ptr<OSWindow> window,
    DecalBuilder* decal_builder,
    LightmapBuilder* lightmap_builder,
    SpellFxRenderer* spellfx,
    std::shared_ptr<ParticleEngine> particle_engine,
    Vis* vis,
    Log* logger
) : RenderBase(window, decal_builder, lightmap_builder, spellfx, particle_engine, vis, logger) {
    clip_w = 0;
    clip_x = 0;
    clip_y = 0;
    clip_z = 0;
}

RenderOpenGL::~RenderOpenGL() { /*__debugbreak();*/ }

void RenderOpenGL::Release() { __debugbreak(); }

void RenderOpenGL::SaveWinnersCertificate(const char *a1) {
    uint winwidth{ window->GetWidth() };
    uint winheight{ window->GetHeight() };
    GLubyte *sPixels = new GLubyte[3 * winwidth * winheight];
    glReadPixels(0, 0, winwidth, winheight, GL_RGB, GL_UNSIGNED_BYTE, sPixels);

    uint16_t *pPixels = (uint16_t *)malloc(sizeof(uint16_t) * winheight * winwidth);
    memset(pPixels, 0, sizeof(uint16_t) * winheight * winwidth);

    // reverse pixels from ogl (uses BL as 0,0)
    uint16_t *for_pixels = pPixels;
    unsigned __int8 *p = sPixels;
    for (uint y = 0; y < (unsigned int)winheight; ++y) {
        for (uint x = 0; x < (unsigned int)winwidth; ++x) {
            p = sPixels + 3 * (int)(x) + 3 * (int)(winheight - y) * winwidth;

            *for_pixels = Color16(*p & 255, *(p + 1) & 255, *(p + 2) & 255);
            ++for_pixels;
        }
    }

    delete[] sPixels;

    SavePCXImage16(a1, (uint16_t *)pPixels, render->GetRenderWidth(), render->GetRenderHeight());
    free(pPixels);
}

void RenderOpenGL::SavePCXImage16(const std::string &filename, uint16_t *picture_data, int width, int height) {
    // TODO(pskelton): add "Screenshots" folder?
    auto thispath = MakeDataPath(filename);
    FILE *result = fopen(thispath.c_str(), "wb");
    if (result == nullptr) {
        return;
    }

    unsigned int pcx_data_size = width * height * 5;
    uint8_t *pcx_data = new uint8_t[pcx_data_size];
    unsigned int pcx_data_real_size = 0;
    PCX::Encode16(picture_data, width, height, pcx_data, pcx_data_size,
        &pcx_data_real_size);
    fwrite(pcx_data, pcx_data_real_size, 1, result);
    delete[] pcx_data;
    fclose(result);
}


bool RenderOpenGL::InitializeFullscreen() {
    __debugbreak();
    return 0;
}

unsigned int RenderOpenGL::GetActorTintColor(int DimLevel, int tint, float WorldViewX, int a5, RenderBillboard *Billboard) {
    // GetActorTintColor(int max_dimm, int min_dimm, float distance, int a4, RenderBillboard *a5)
    return ::GetActorTintColor(DimLevel, tint, WorldViewX, a5, Billboard);
}


// when losing and regaining window focus - not required for OGL??
void RenderOpenGL::RestoreFrontBuffer() { /*__debugbreak();*/ }
void RenderOpenGL::RestoreBackBuffer() { /*__debugbreak();*/ }

void RenderOpenGL::BltBackToFontFast(int a2, int a3, Rect *a4) {
    __debugbreak();  // never called anywhere
}



unsigned int RenderOpenGL::GetRenderWidth() const { return window->GetWidth(); }
unsigned int RenderOpenGL::GetRenderHeight() const { return window->GetHeight(); }

void RenderOpenGL::ClearBlack() {  // used only at start and in game over win
    ClearZBuffer();
    ClearTarget(0);
}

void RenderOpenGL::ClearTarget(unsigned int uColor) {
    return;
}



void RenderOpenGL::CreateZBuffer() {
    if (!pActiveZBuffer) {
        pActiveZBuffer = (int*)malloc(window->GetWidth() * window->GetHeight() * sizeof(int));
        ClearZBuffer();
    }
}

void RenderOpenGL::ClearZBuffer() {
    memset32(this->pActiveZBuffer, 0xFFFF0000, window->GetWidth() * window->GetHeight());
}

void RenderOpenGL::RasterLine2D(signed int uX, signed int uY, signed int uZ,
                                signed int uW, unsigned __int16 uColor) {
    unsigned int b = (uColor & 0x1F)*8;
    unsigned int g = ((uColor >> 5) & 0x3F)*4;
    unsigned int r = ((uColor >> 11) & 0x1F)*8;

    glDisable(GL_TEXTURE_2D);
    glLineWidth(1);
    glColor3ub(r, g, b);

    // pixel centers around 0.5 so tweak to avoid gaps and squashing
    if (uZ == uX) {
       uW += 1;
    }

    glBegin(GL_LINES);
    glVertex3f(uX, uY, 0);
    glVertex3f(uZ+.5, uW+.5, 0);
    drawcalls++;
    glEnd();

    GL_Check_Errors();
}

void RenderOpenGL::BeginSceneD3D() {
    // Setup for 3D

    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);

    glClearColor(0, 0, 0, 0/*0.9f, 0.5f, 0.1f, 1.0f*/);
    glClearDepth(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    render->uNumBillboardsToDraw = 0;  // moved from drawbillboards - cant reset this until mouse picking finished
    GL_Check_Errors();
}

extern unsigned int BlendColors(unsigned int a1, unsigned int a2);

void RenderOpenGL::DrawBillboard_Indoor(SoftwareBillboard *pSoftBillboard,
                                        RenderBillboard *billboard) {
    int v11;     // eax@9
    int v12;     // eax@9
    double v15;  // st5@12
    double v16;  // st4@12
    double v17;  // st3@12
    double v18;  // st2@12
    int v19;     // ecx@14
    double v20;  // st3@14
    int v21;     // ecx@16
    double v22;  // st3@16
    float v27;   // [sp+24h] [bp-Ch]@5
    float v29;   // [sp+2Ch] [bp-4h]@5
    float v31;   // [sp+3Ch] [bp+Ch]@5
    float a1;    // [sp+40h] [bp+10h]@5

    // if (this->uNumD3DSceneBegins == 0) {
    //    return;
    //}

    Sprite *pSprite = billboard->hwsprite;
    int dimming_level = billboard->dimming_level;

    // v4 = pSoftBillboard;
    // v5 = (double)pSoftBillboard->zbuffer_depth;
    // pSoftBillboarda = pSoftBillboard->zbuffer_depth;
    // v6 = pSoftBillboard->zbuffer_depth;
    unsigned int v7 = Billboard_ProbablyAddToListAndSortByZOrder(
        pSoftBillboard->screen_space_z);
    // v8 = dimming_level;
    // device_caps = v7;
    int v28 = dimming_level & 0xFF000000;
    if (dimming_level & 0xFF000000) {
        pBillboardRenderListD3D[v7].opacity = RenderBillboardD3D::Opaque_3;
    } else {
        pBillboardRenderListD3D[v7].opacity = RenderBillboardD3D::Transparent;
    }
    // v10 = a3;
    pBillboardRenderListD3D[v7].field_90 = pSoftBillboard->field_44;
    pBillboardRenderListD3D[v7].screen_space_z = pSoftBillboard->screen_space_z;
    pBillboardRenderListD3D[v7].object_pid = pSoftBillboard->object_pid;
    pBillboardRenderListD3D[v7].sParentBillboardID =
        pSoftBillboard->sParentBillboardID;
    // v25 = pSoftBillboard->uScreenSpaceX;
    // v24 = pSoftBillboard->uScreenSpaceY;
    a1 = pSoftBillboard->screenspace_projection_factor_x;
    v29 = pSoftBillboard->screenspace_projection_factor_y;
    v31 = (double)((pSprite->uBufferWidth >> 1) - pSprite->uAreaX);
    v27 = (double)(pSprite->uBufferHeight - pSprite->uAreaY);
    if (pSoftBillboard->uFlags & 4) {
        v31 = v31 * -1.0;
    }
    if (config->is_tinting && pSoftBillboard->sTintColor) {
        v11 = ::GetActorTintColor(dimming_level, 0,
            pSoftBillboard->screen_space_z, 0, 0);
        v12 = BlendColors(pSoftBillboard->sTintColor, v11);
        if (v28)
            v12 =
            (uint64_t)((char *)&array_77EC08[1852].pEdgeList1[17] + 3) &
            ((unsigned int)v12 >> 1);
    } else {
        v12 = ::GetActorTintColor(dimming_level, 0,
            pSoftBillboard->screen_space_z, 0, 0);
    }
    // v13 = (double)v25;
    pBillboardRenderListD3D[v7].pQuads[0].specular = 0;
    pBillboardRenderListD3D[v7].pQuads[0].diffuse = v12;
    pBillboardRenderListD3D[v7].pQuads[0].pos.x =
        pSoftBillboard->screen_space_x - v31 * a1;
    // v14 = (double)v24;
    // v32 = v14;
    pBillboardRenderListD3D[v7].pQuads[0].pos.y =
        pSoftBillboard->screen_space_y - v27 * v29;
    v15 = 1.0 - 1.0 / (pSoftBillboard->screen_space_z * 0.061758894);
    pBillboardRenderListD3D[v7].pQuads[0].pos.z = v15;
    v16 = 1.0 / pSoftBillboard->screen_space_z;
    pBillboardRenderListD3D[v7].pQuads[0].rhw =
        1.0 / pSoftBillboard->screen_space_z;
    pBillboardRenderListD3D[v7].pQuads[0].texcoord.x = 0.0;
    pBillboardRenderListD3D[v7].pQuads[0].texcoord.y = 0.0;
    v17 = (double)((pSprite->uBufferWidth >> 1) - pSprite->uAreaX);
    v18 = (double)(pSprite->uBufferHeight - pSprite->uAreaY -
        pSprite->uAreaHeight);
    if (pSoftBillboard->uFlags & 4) {
        v17 = v17 * -1.0;
    }
    pBillboardRenderListD3D[v7].pQuads[1].specular = 0;
    pBillboardRenderListD3D[v7].pQuads[1].diffuse = v12;
    pBillboardRenderListD3D[v7].pQuads[1].pos.x =
        pSoftBillboard->screen_space_x - v17 * a1;
    pBillboardRenderListD3D[v7].pQuads[1].pos.y =
        pSoftBillboard->screen_space_y - v18 * v29;
    pBillboardRenderListD3D[v7].pQuads[1].pos.z = v15;
    pBillboardRenderListD3D[v7].pQuads[1].rhw = v16;
    pBillboardRenderListD3D[v7].pQuads[1].texcoord.x = 0.0;
    pBillboardRenderListD3D[v7].pQuads[1].texcoord.y = 1.0;
    v19 = pSprite->uBufferHeight - pSprite->uAreaY - pSprite->uAreaHeight;
    v20 = (double)(pSprite->uAreaX + pSprite->uAreaWidth +
        (pSprite->uBufferWidth >> 1) - pSprite->uBufferWidth);
    if (pSoftBillboard->uFlags & 4) {
        v20 = v20 * -1.0;
    }
    pBillboardRenderListD3D[v7].pQuads[2].specular = 0;
    pBillboardRenderListD3D[v7].pQuads[2].diffuse = v12;
    pBillboardRenderListD3D[v7].pQuads[2].pos.x =
        v20 * a1 + pSoftBillboard->screen_space_x;
    pBillboardRenderListD3D[v7].pQuads[2].pos.y =
        pSoftBillboard->screen_space_y - (double)v19 * v29;
    pBillboardRenderListD3D[v7].pQuads[2].pos.z = v15;
    pBillboardRenderListD3D[v7].pQuads[2].rhw = v16;
    pBillboardRenderListD3D[v7].pQuads[2].texcoord.x = 1.0;
    pBillboardRenderListD3D[v7].pQuads[2].texcoord.y = 1.0;
    v21 = pSprite->uBufferHeight - pSprite->uAreaY;
    v22 = (double)(pSprite->uAreaX + pSprite->uAreaWidth +
        (pSprite->uBufferWidth >> 1) - pSprite->uBufferWidth);
    if (pSoftBillboard->uFlags & 4) {
        v22 = v22 * -1.0;
    }
    pBillboardRenderListD3D[v7].pQuads[3].specular = 0;
    pBillboardRenderListD3D[v7].pQuads[3].diffuse = v12;
    pBillboardRenderListD3D[v7].pQuads[3].pos.x =
        v22 * a1 + pSoftBillboard->screen_space_x;
    pBillboardRenderListD3D[v7].pQuads[3].pos.y =
        pSoftBillboard->screen_space_y - (double)v21 * v29;
    pBillboardRenderListD3D[v7].pQuads[3].pos.z = v15;
    pBillboardRenderListD3D[v7].pQuads[3].rhw = v16;
    pBillboardRenderListD3D[v7].pQuads[3].texcoord.x = 1.0;
    pBillboardRenderListD3D[v7].pQuads[3].texcoord.y = 0.0;
    // v23 = pSprite->pTexture;
    pBillboardRenderListD3D[v7].uNumVertices = 4;
    pBillboardRenderListD3D[v7].z_order = pSoftBillboard->screen_space_z;
    pBillboardRenderListD3D[v7].texture = pSprite->texture;
}

//----- (004A4CC9) ---------------------------------------
void RenderOpenGL::BillboardSphereSpellFX(struct SpellFX_Billboard *a1, int diffuse) {
    // fireball / implosion sphere
    //__debugbreak();

    // TODO(pskelton): could draw in 3d rather than convert to billboard for ogl

    if (a1->uNumVertices < 3) {
        return;
    }

    float depth = 1000000.0;
    for (uint i = 0; i < (unsigned int)a1->uNumVertices; ++i) {
        if (a1->field_104[i].z < depth) {
            depth = a1->field_104[i].z;
        }
    }

    unsigned int v5 = Billboard_ProbablyAddToListAndSortByZOrder(depth);
    pBillboardRenderListD3D[v5].field_90 = 0;
    pBillboardRenderListD3D[v5].sParentBillboardID = -1;
    pBillboardRenderListD3D[v5].opacity = RenderBillboardD3D::Opaque_2;
    pBillboardRenderListD3D[v5].texture = 0;
    pBillboardRenderListD3D[v5].uNumVertices = a1->uNumVertices;
    pBillboardRenderListD3D[v5].z_order = depth;

    for (unsigned int i = 0; i < (unsigned int)a1->uNumVertices; ++i) {
        pBillboardRenderListD3D[v5].pQuads[i].pos.x = a1->field_104[i].x;
        pBillboardRenderListD3D[v5].pQuads[i].pos.y = a1->field_104[i].y;
        pBillboardRenderListD3D[v5].pQuads[i].pos.z = a1->field_104[i].z;

        float rhw = 1.f / a1->field_104[i].z;
        float z = 1.f - 1.f / (a1->field_104[i].z * 1000.f / pCamera3D->GetFarClip());

        double v10 = a1->field_104[i].z;
        v10 *= 1000.f / pCamera3D->GetFarClip();

        pBillboardRenderListD3D[v5].pQuads[i].rhw = rhw;

        int v12;
        if (diffuse & 0xFF000000) {
            v12 = a1->field_104[i].diffuse;
        } else {
            v12 = diffuse;
        }
        pBillboardRenderListD3D[v5].pQuads[i].diffuse = v12;
        pBillboardRenderListD3D[v5].pQuads[i].specular = 0;

        pBillboardRenderListD3D[v5].pQuads[i].texcoord.x = 0.0;
        pBillboardRenderListD3D[v5].pQuads[i].texcoord.y = 0.0;
    }
}

void RenderOpenGL::DrawBillboardList_BLV() {
    SoftwareBillboard soft_billboard = { 0 };
    soft_billboard.sParentBillboardID = -1;
    //  soft_billboard.pTarget = pBLVRenderParams->pRenderTarget;
    soft_billboard.pTargetZ = pBLVRenderParams->pTargetZBuffer;
    //  soft_billboard.uTargetPitch = uTargetSurfacePitch;
    soft_billboard.uViewportX = pBLVRenderParams->uViewportX;
    soft_billboard.uViewportY = pBLVRenderParams->uViewportY;
    soft_billboard.uViewportZ = pBLVRenderParams->uViewportZ - 1;
    soft_billboard.uViewportW = pBLVRenderParams->uViewportW;

    pODMRenderParams->uNumBillboards = ::uNumBillboardsToDraw;
    for (uint i = 0; i < ::uNumBillboardsToDraw; ++i) {
        RenderBillboard *p = &pBillboardRenderList[i];
        if (p->hwsprite) {
            soft_billboard.screen_space_x = p->screen_space_x;
            soft_billboard.screen_space_y = p->screen_space_y;
            soft_billboard.screen_space_z = p->screen_space_z;
            soft_billboard.sParentBillboardID = i;
            soft_billboard.screenspace_projection_factor_x =
                p->screenspace_projection_factor_x;
            soft_billboard.screenspace_projection_factor_y =
                p->screenspace_projection_factor_y;
            soft_billboard.object_pid = p->object_pid;
            soft_billboard.uFlags = p->field_1E;
            soft_billboard.sTintColor = p->sTintColor;

            DrawBillboard_Indoor(&soft_billboard, p);
        }
    }
}


void RenderOpenGL::DrawProjectile(float srcX, float srcY, float a3, float a4,
                                  float dstX, float dstY, float a7, float a8,
                                  Texture *texture) {
    // a3 - src worldviewx
    // a4 - src fov / worldview
    // a7 - dst worldview
    // a8 - dst fov / worldview

    // TODO(pskelton): fix properly - sometimes half disappears

    // billboards projectile - lightning bolt

    double v20;  // st4@8
    double v21;  // st4@10
    double v22;  // st4@10
    double v23;  // st4@10
    double v25;  // st4@11
    double v26;  // st4@13
    double v28;  // st4@13


    TextureOpenGL *textured3d = (TextureOpenGL *)texture;

    int xDifference = bankersRounding(dstX - srcX);
    int yDifference = bankersRounding(dstY - srcY);
    int absYDifference = abs(yDifference);
    int absXDifference = abs(xDifference);
    unsigned int smallerabsdiff = std::min(absXDifference, absYDifference);
    unsigned int largerabsdiff = std::max(absXDifference, absYDifference);

    // distance approx
    int v32 = (11 * smallerabsdiff >> 5) + largerabsdiff;

    double v16 = 1.0 / (double)v32;
    double srcxmod = (double)yDifference * v16 * a4;
    double srcymod = (double)xDifference * v16 * a4;

    v20 = a3 * 1000.0 / pCamera3D->GetFarClip();
    v25 = a7 * 1000.0 / pCamera3D->GetFarClip();

    v21 = 1.0 / a3;
    v22 = (double)yDifference * v16 * a8;
    v23 = (double)xDifference * v16 * a8;
    v26 = 1.0 - 1.0 / v25;
    v28 = 1.0 / a7;

    RenderVertexD3D3 v29[4];
    v29[0].pos.x = srcX + srcxmod;
    v29[0].pos.y = srcY - srcymod;
    v29[0].pos.z = 1.0 - 1.0 / v20;
    v29[0].rhw = v21;
    v29[0].diffuse = -1;
    v29[0].specular = 0;
    v29[0].texcoord.x = 1.0;
    v29[0].texcoord.y = 0.0;

    v29[1].pos.x = v22 + dstX;
    v29[1].pos.y = dstY - v23;
    v29[1].pos.z = v26;
    v29[1].rhw = v28;
    v29[1].diffuse = -16711936;
    v29[1].specular = 0;
    v29[1].texcoord.x = 1.0;
    v29[1].texcoord.y = 1.0;

    v29[2].pos.x = dstX - v22;
    v29[2].pos.y = v23 + dstY;
    v29[2].pos.z = v26;
    v29[2].rhw = v28;
    v29[2].diffuse = -1;
    v29[2].specular = 0;
    v29[2].texcoord.x = 0.0;
    v29[2].texcoord.y = 1.0;

    v29[3].pos.x = srcX - srcxmod;
    v29[3].pos.y = srcymod + srcY;
    v29[3].pos.z = v29[0].pos.z;
    v29[3].rhw = v21;
    v29[3].diffuse = -1;
    v29[3].specular = 0;
    v29[3].texcoord.x = 0.0;
    v29[3].texcoord.y = 0.0;


    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    // ErrD3D(pRenderD3D->pDevice->SetRenderState(D3DRENDERSTATE_DITHERENABLE, FALSE));
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    if (textured3d) {
        glBindTexture(GL_TEXTURE_2D, textured3d->GetOpenGlTexture());
    } else {
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glBegin(GL_TRIANGLE_FAN);

    for (uint i = 0; i < 4; ++i) {
        glColor4f(1, 1, 1, 1.0f);  // ????
        glTexCoord2f(v29[i].texcoord.x, v29[i].texcoord.y);
        glVertex3f(v29[i].pos.x, v29[i].pos.y, v29[i].pos.z);
    }



    glEnd();

    drawcalls++;

    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);


    //ErrD3D(pRenderD3D->pDevice->SetRenderState(D3DRENDERSTATE_DITHERENABLE, TRUE));
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);

    GL_Check_Errors();
}

void RenderOpenGL::ScreenFade(unsigned int color, float t) { __debugbreak(); }


void RenderOpenGL::DrawTextureOffset(int pX, int pY, int move_X, int move_Y,
                                     Image *pTexture) {
    DrawTextureNew((float)(pX - move_X)/window->GetWidth(), (float)(pY - move_Y)/window->GetHeight(), pTexture);
}


void RenderOpenGL::DrawImage(Image *img, const Rect &rect) {
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    bool blendFlag = 1;

    // check if loaded
    auto texture = (TextureOpenGL *)img;
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    if (blendFlag) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    float depth = 0;

    GLfloat Vertices[] = { (float)rect.x, (float)rect.y, depth,
        (float)rect.z, (float)rect.y, depth,
        (float)rect.z, (float)rect.w, depth,
        (float)rect.x, (float)rect.w, depth };

    GLfloat TexCoord[] = { 0, 0,
        1, 0,
        1, 1,
        0, 1 };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
        0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, TexCoord);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_BLEND);

    GL_Check_Errors();
}


void RenderOpenGL::ZDrawTextureAlpha(float u, float v, Image *img, int zVal) {
    if (!img) return;

    int winwidth = this->window->GetWidth();
    int uOutX = u * winwidth;
    int uOutY = v * this->window->GetHeight();
    unsigned int imgheight = img->GetHeight();
    unsigned int imgwidth = img->GetWidth();
    auto pixels = (uint32_t *)img->GetPixels(IMAGE_FORMAT_A8R8G8B8);

    if (uOutX < 0)
        uOutX = 0;
    if (uOutY < 0)
        uOutY = 0;

    for (int xs = 0; xs < imgwidth; xs++) {
        for (int ys = 0; ys < imgheight; ys++) {
            if (pixels[xs + imgwidth * ys] & 0xFF000000) {
                this->pActiveZBuffer[uOutX + xs + winwidth * (uOutY + ys)] = zVal;
            }
        }
    }
}



// TODO(pskelton): stencil masking with opacity would be a better way to do this
void RenderOpenGL::BlendTextures(int x, int y, Image* imgin, Image* imgblend, int time, int start_opacity,
    int end_opacity) {
    // thrown together as a crude estimate of the enchaintg effects
    // leaves gap where it shouldnt on dark pixels currently
    // doesnt use opacity params

    const uint32_t* pixelpoint;
    const uint32_t* pixelpointblend;

    if (imgin && imgblend) {  // 2 images to blend
        pixelpoint = (const uint32_t*)imgin->GetPixels(IMAGE_FORMAT_A8R8G8B8);
        pixelpointblend =
            (const uint32_t*)imgblend->GetPixels(IMAGE_FORMAT_A8R8G8B8);

        int Width = imgin->GetWidth();
        int Height = imgin->GetHeight();
        Texture *temp = render->CreateTexture_Blank(Width, Height, IMAGE_FORMAT_A8R8G8B8);
        //Image* temp = Image::Create(Width, Height, IMAGE_FORMAT_A8R8G8B8);
        uint32_t* temppix = (uint32_t*)temp->GetPixels(IMAGE_FORMAT_A8R8G8B8);

        uint32_t c = *(pixelpointblend + 2700);  // guess at brightest pixel
        unsigned int bmax = (c & 0xFF);
        unsigned int gmax = ((c >> 8) & 0xFF);
        unsigned int rmax = ((c >> 16) & 0xFF);

        unsigned int bmin = bmax / 10;
        unsigned int gmin = gmax / 10;
        unsigned int rmin = rmax / 10;

        unsigned int bstep = (bmax - bmin) / 128;
        unsigned int gstep = (gmax - gmin) / 128;
        unsigned int rstep = (rmax - rmin) / 128;

        for (int ydraw = 0; ydraw < Height; ++ydraw) {
            for (int xdraw = 0; xdraw < Width; ++xdraw) {
                // should go blue -> black -> blue reverse
                // patchy -> solid -> patchy

                if (*pixelpoint) {  // check orig item not got blakc pixel
                    uint32_t nudge =
                        (xdraw % imgblend->GetWidth()) +
                        (ydraw % imgblend->GetHeight()) * imgblend->GetWidth();
                    uint32_t pixcol = *(pixelpointblend + nudge);

                    unsigned int bcur = (pixcol & 0xFF);
                    unsigned int gcur = ((pixcol >> 8) & 0xFF);
                    unsigned int rcur = ((pixcol >> 16) & 0xFF);

                    int steps = (time) % 128;

                    if ((time) % 256 >= 128) {  // step down
                        bcur += bstep * (128 - steps);
                        gcur += gstep * (128 - steps);
                        rcur += rstep * (128 - steps);
                    } else {  // step up
                        bcur += bstep * steps;
                        gcur += gstep * steps;
                        rcur += rstep * steps;
                    }

                    if (bcur > bmax) bcur = bmax;  // limit check
                    if (gcur > gmax) gcur = gmax;
                    if (rcur > rmax) rcur = rmax;
                    if (bcur < bmin) bcur = bmin;
                    if (gcur < gmin) gcur = gmin;
                    if (rcur < rmin) rcur = rmin;

                    temppix[xdraw + ydraw * Width] = Color32(rcur, gcur, bcur);
                }

                pixelpoint++;
            }

            pixelpoint += imgin->GetWidth() - Width;
        }
        // draw image
        render->Update_Texture(temp);
        render->DrawTextureAlphaNew(x / float(window->GetWidth()), y / float(window->GetHeight()), temp);
        temp->Release();
    }
}


void RenderOpenGL::TexturePixelRotateDraw(float u, float v, Image *img, int time) {
    if (img) {
        auto pixelpoint = (const uint32_t *)img->GetPixels(IMAGE_FORMAT_A8R8G8B8);
        int width = img->GetWidth();
        int height = img->GetHeight();
        Texture *temp = CreateTexture_Blank(width, height, IMAGE_FORMAT_A8R8G8B8);
        uint32_t *temppix = (uint32_t *)temp->GetPixels(IMAGE_FORMAT_A8R8G8B8);

        int brightloc = -1;
        int brightval = 0;
        int darkloc = -1;
        int darkval = 765;

        for (int x = 0; x < width; x++) {
            for (int y = 0; y < height; y++) {
                int nudge = x + y * width;
                // Test the brightness against the threshold
                int bright = (*(pixelpoint + nudge) & 0xFF) + ((*(pixelpoint + nudge) >> 8) & 0xFF) + ((*(pixelpoint + nudge) >> 16) & 0xFF);
                if (bright == 0) continue;

                if (bright > brightval) {
                    brightval = bright;
                    brightloc = nudge;
                }
                if (bright < darkval) {
                    darkval = bright;
                    darkloc = nudge;
                }
            }
        }

        // find brightest
        unsigned int bmax = (*(pixelpoint + brightloc) & 0xFF);
        unsigned int gmax = ((*(pixelpoint + brightloc) >> 8) & 0xFF);
        unsigned int rmax = ((*(pixelpoint + brightloc) >> 16) & 0xFF);

        // find darkest not black
        unsigned int bmin = (*(pixelpoint + darkloc) & 0xFF);
        unsigned int gmin = ((*(pixelpoint + darkloc) >> 8) & 0xFF);
        unsigned int rmin = ((*(pixelpoint + darkloc) >> 16) & 0xFF);

        // steps pixels
        float bstep = (bmax - bmin) / 128.;
        float gstep = (gmax - gmin) / 128.;
        float rstep = (rmax - rmin) / 128.;

        int timestep = time % 256;

        // loop through
        for (int ydraw = 0; ydraw < height; ++ydraw) {
            for (int xdraw = 0; xdraw < width; ++xdraw) {
                if (*pixelpoint) {  // check orig item not got blakc pixel
                    unsigned int bcur = (*(pixelpoint) & 0xFF);
                    unsigned int gcur = ((*(pixelpoint) >> 8) & 0xFF);
                    unsigned int rcur = ((*(pixelpoint) >> 16) & 0xFF);
                    int pixstepb = (bcur - bmin) / bstep + timestep;
                    if (pixstepb > 255) pixstepb = pixstepb - 256;
                    if (pixstepb >= 0 && pixstepb < 128)  // 0-127
                        bcur = bmin + pixstepb * bstep;
                    if (pixstepb >= 128 && pixstepb < 256) {  // 128-255
                        pixstepb = pixstepb - 128;
                        bcur = bmax - pixstepb * bstep;
                    }
                    int pixstepr = (rcur - rmin) / rstep + timestep;
                    if (pixstepr > 255) pixstepr = pixstepr - 256;
                    if (pixstepr >= 0 && pixstepr < 128)  // 0-127
                        rcur = rmin + pixstepr * rstep;
                    if (pixstepr >= 128 && pixstepr < 256) {  // 128-255
                        pixstepr = pixstepr - 128;
                        rcur = rmax - pixstepr * rstep;
                    }
                    int pixstepg = (gcur - gmin) / gstep + timestep;
                    if (pixstepg > 255) pixstepg = pixstepg - 256;
                    if (pixstepg >= 0 && pixstepg < 128)  // 0-127
                        gcur = gmin + pixstepg * gstep;
                    if (pixstepg >= 128 && pixstepg < 256) {  // 128-255
                        pixstepg = pixstepg - 128;
                        gcur = gmax - pixstepg * gstep;
                    }
                    // out pixel
                    temppix[xdraw + ydraw * width] = Color32(rcur, gcur, bcur);
                }
                pixelpoint++;
            }
        }
        // draw image
        render->Update_Texture(temp);
        render->DrawTextureAlphaNew(u, v, temp);
        temp->Release();
    }
}



void RenderOpenGL::DrawMonsterPortrait(Rect rc, SpriteFrame *Portrait, int Y_Offset) {
    Rect rct;
    rct.x = rc.x + 64 + Portrait->hw_sprites[0]->uAreaX - Portrait->hw_sprites[0]->uBufferWidth / 2;
    rct.y = rc.y + Y_Offset + Portrait->hw_sprites[0]->uAreaY;
    rct.z = rct.x + Portrait->hw_sprites[0]->uAreaWidth;
    rct.w = rct.y + Portrait->hw_sprites[0]->uAreaHeight;

    render->SetUIClipRect(rc.x, rc.y, rc.z, rc.w);
    render->DrawImage(Portrait->hw_sprites[0]->texture, rct);
    render->ResetUIClipRect();
}

void RenderOpenGL::DrawTransparentRedShade(float u, float v, Image *a4) {
    DrawMasked(u, v, a4, 0, 0xF800);
}

void RenderOpenGL::DrawTransparentGreenShade(float u, float v, Image *pTexture) {
    DrawMasked(u, v, pTexture, 0, 0x07E0);
}

//void RenderOpenGL::DrawFansTransparent(const RenderVertexD3D3 *vertices,
//                                       unsigned int num_vertices) {
//    __debugbreak();
//}

void RenderOpenGL::DrawMasked(float u, float v, Image *pTexture, unsigned int color_dimming_level, unsigned __int16 mask) {
    uint col = Color32(255, 255, 255);

    if (mask)
        col = Color32(mask);

    float r = ((col >> 16) & 0xFF) & (0xFF>> color_dimming_level);
    float g = ((col >> 8) & 0xFF) & (0xFF >> color_dimming_level);
    float b = ((col) & 0xFF) & (0xFF >> color_dimming_level);

    col = Color32(r, g, b);

    DrawTextureNew(u, v, pTexture, col);
    return;
}



void RenderOpenGL::DrawTextureGrayShade(float a2, float a3, Image *a4) {
    DrawMasked(a2, a3, a4, 1, 0x7BEF);
}

void RenderOpenGL::DrawIndoorSky(unsigned int uNumVertices, unsigned int uFaceID) {
    // TODO(pskelton): fix properly - only partially works
    // for floor and wall(for example Celeste)-------------------
    BLVFace *pFace = &pIndoor->pFaces[uFaceID];
    if (pFace->uPolygonType == POLYGON_InBetweenFloorAndWall || pFace->uPolygonType == POLYGON_Floor) {
        int v69 = (OS_GetTime() / 32) - pCamera3D->vCameraPos.x;
        int v55 = (OS_GetTime() / 32) + pCamera3D->vCameraPos.y;
        for (uint i = 0; i < uNumVertices; ++i) {
            array_507D30[i].u = (v69 + array_507D30[i].u) * 0.25f;
            array_507D30[i].v = (v55 + array_507D30[i].v) * 0.25f;
        }
        render->DrawIndoorPolygon(uNumVertices, pFace, PID(OBJECT_BModel, uFaceID), -1, 0);
        return;
    }
    //---------------------------------------

    // TODO(pskelton): temporary hack to use outdoor sky as a coverall instead of drawing the individual segments which causes hazy transitions without rhw correction
    SkyBillboard.CalcSkyFrustumVec(1, 0, 0, 0, 1, 0);
    render->DrawOutdoorSkyD3D();
    return;


    if ((signed int)uNumVertices <= 0) return;

    struct Polygon pSkyPolygon;
    pSkyPolygon.texture = nullptr;
    pSkyPolygon.texture = pFace->GetTexture();
    if (!pSkyPolygon.texture) return;

    pSkyPolygon.ptr_38 = &SkyBillboard;
    pSkyPolygon.dimming_level = 0;
    pSkyPolygon.uNumVertices = uNumVertices;

    SkyBillboard.CalcSkyFrustumVec(1, 0, 0, 0, 1, 0);

    double rot_to_rads = ((2 * pi_double) / 2048);

    // lowers clouds as party goes up
    float  blv_horizon_height_offset = ((double)(pCamera3D->ViewPlaneDist_X * pCamera3D->vCameraPos.z)
        / ((double)pCamera3D->ViewPlaneDist_X + pCamera3D->GetFarClip())
        + (double)(pBLVRenderParams->uViewportCenterY));

    double cam_y_rot_rad = (double)pCamera3D->sRotationY * rot_to_rads;

    float depth_to_far_clip = cos((double)pCamera3D->sRotationY * rot_to_rads) * pCamera3D->GetFarClip();
    float height_to_far_clip = sin((double)pCamera3D->sRotationY * rot_to_rads) * pCamera3D->GetFarClip();

    float blv_bottom_y_proj = ((double)(pBLVRenderParams->uViewportCenterY) -
        (double)pCamera3D->ViewPlaneDist_X /
        (depth_to_far_clip + 0.0000001) *
        (height_to_far_clip - (double)pCamera3D->vCameraPos.z));

    // rotation vec for sky plane - pitch
    float v_18x = -sin((-pCamera3D->sRotationY + 16) * rot_to_rads);
    float v_18y = 0.0f;
    float v_18z = -cos((pCamera3D->sRotationY + 16) * rot_to_rads);

    float inv_viewplanedist = 1.0f / pCamera3D->ViewPlaneDist_X;

    int _507D30_idx = 0;
    for (_507D30_idx; _507D30_idx < pSkyPolygon.uNumVertices; _507D30_idx++) {
        // outbound screen x dist
        float x_dist = inv_viewplanedist * (pBLVRenderParams->uViewportCenterX - array_507D30[_507D30_idx].vWorldViewProjX);
        // outbound screen y dist
        float y_dist = inv_viewplanedist * (blv_horizon_height_offset - array_507D30[_507D30_idx].vWorldViewProjY);

        // rotate vectors to cam facing
        float skyfinalleft = (pSkyPolygon.ptr_38->CamVecLeft_X * x_dist) + (pSkyPolygon.ptr_38->CamVecLeft_Z * y_dist) + pSkyPolygon.ptr_38->CamVecLeft_Y;
        float skyfinalfront = (pSkyPolygon.ptr_38->CamVecFront_X * x_dist) + (pSkyPolygon.ptr_38->CamVecFront_Z * y_dist) + pSkyPolygon.ptr_38->CamVecFront_Y;

        // pitch rotate sky to get top projection
        float newX = v_18x + v_18y + (v_18z * y_dist);
        float worldviewdepth = -512.0f / newX;

        // offset tex coords
        float texoffset_U = (float(pMiscTimer->uTotalGameTimeElapsed) / 128.0) + ((skyfinalleft * worldviewdepth) / 16.0f);
        array_507D30[_507D30_idx].u = texoffset_U / ((float)pSkyPolygon.texture->GetWidth());
        float texoffset_V = (float(pMiscTimer->uTotalGameTimeElapsed) / 128.0) + ((skyfinalfront * worldviewdepth) / 16.0f);
        array_507D30[_507D30_idx].v = texoffset_V / ((float)pSkyPolygon.texture->GetHeight());

        // this basically acts as texture perspective correction
        array_507D30[_507D30_idx]._rhw = /*1.0f /*/ (double)worldviewdepth;
    }

    // no clipped polygon so draw and return??
    if (_507D30_idx >= pSkyPolygon.uNumVertices) {
        DrawIndoorSkyPolygon(pSkyPolygon.uNumVertices, &pSkyPolygon);
        return;
    }

    logger->Info("past normal section");
    __debugbreak();
    // please provide save game / details if you get here
    // TODO(pskelton): below looks like some vert clipping but dont think its ever gets here now - delete below after testing;
}

void RenderOpenGL::DrawIndoorSkyPolygon(signed int uNumVertices, struct Polygon *pSkyPolygon) {
    TextureOpenGL *texture = (TextureOpenGL *)pSkyPolygon->texture;

    //if (uNumD3DSceneBegins == 0) {
    //    return;
    //}

    if (uNumVertices >= 3) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        int v5 = 31 - (pSkyPolygon->dimming_level & 0x1F);
        if (v5 < pOutdoor->max_terrain_dimming_level) {
            v5 = pOutdoor->max_terrain_dimming_level;
        }

        for (uint i = 0; i < (unsigned int)uNumVertices; ++i) {
            d3d_vertex_buffer[i].pos.x = array_507D30[i].vWorldViewProjX;
            d3d_vertex_buffer[i].pos.y = array_507D30[i].vWorldViewProjY;
            d3d_vertex_buffer[i].pos.z =
                1.0 -
                1.0 / (array_507D30[i].vWorldViewPosition.x * 0.061758894);
            d3d_vertex_buffer[i].rhw = 1.0 / array_507D30[i]._rhw;
            d3d_vertex_buffer[i].diffuse =
                8 * v5 | ((8 * v5 | (v5 << 11)) << 8);
            d3d_vertex_buffer[i].specular = 0;
            d3d_vertex_buffer[i].texcoord.x = array_507D30[i].u;
            d3d_vertex_buffer[i].texcoord.y = array_507D30[i].v;
        }

        glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

        glBegin(GL_TRIANGLE_FAN);

        for (uint i = 0; i < uNumVertices; ++i) {
            glColor4f((8 * v5) / 255.0f, (8 * v5) / 255.0f, (8 * v5) / 255.0f, 1.0f);
            glTexCoord2f(d3d_vertex_buffer[i].texcoord.x, d3d_vertex_buffer[i].texcoord.y);
            glVertex3f(array_507D30[i].vWorldPosition.x, array_507D30[i].vWorldPosition.y, array_507D30[i].vWorldPosition.z);
        }

        glEnd();

        drawcalls++;

        //if (engine->config->debug_terrain)
        //    pCamera3D->debug_outline_d3d(d3d_vertex_buffer, uNumVertices, 0x00FF0000, 0.0);
    }
}

bool RenderOpenGL::AreRenderSurfacesOk() { return true; }

unsigned short *RenderOpenGL::MakeScreenshot16(int width, int height) {
    BeginSceneD3D();

    if (uCurrentlyLoadedLevelType == LEVEL_Indoor) {
        pIndoor->Draw();
    } else if (uCurrentlyLoadedLevelType == LEVEL_Outdoor) {
        pOutdoor->Draw();
    }

    DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene();

    GLubyte *sPixels = new GLubyte[3 * window->GetWidth() * window->GetHeight()];
    glReadPixels(0, 0, window->GetWidth(), window->GetHeight(), GL_RGB, GL_UNSIGNED_BYTE, sPixels);

    int interval_x = game_viewport_width / (double)width;
    int interval_y = game_viewport_height / (double)height;

    uint16_t *pPixels = (uint16_t *)malloc(sizeof(uint16_t) * height * width);
    memset(pPixels, 0, sizeof(uint16_t) * height * width);

    uint16_t *for_pixels = pPixels;

    if (uCurrentlyLoadedLevelType == LEVEL_null) {
        memset(&for_pixels, 0, sizeof(for_pixels));
    } else {
        for (uint y = 0; y < (unsigned int)height; ++y) {
            for (uint x = 0; x < (unsigned int)width; ++x) {
                unsigned __int8 *p;

                p = sPixels + 3 * (int)(x * interval_x + 8.0) + 3 * (int)(window->GetHeight() - (y * interval_y) - 8.0) * window->GetWidth();

                *for_pixels = Color16(*p & 255, *(p + 1) & 255, *(p + 2) & 255);
                ++for_pixels;
            }
        }
    }

    delete [] sPixels;
    return pPixels;
}

Image *RenderOpenGL::TakeScreenshot(unsigned int width, unsigned int height) {
    auto pixels = MakeScreenshot16(width, height);
    Image *image = Image::Create(width, height, IMAGE_FORMAT_R5G6B5, pixels);
    free(pixels);
    return image;
}

void RenderOpenGL::SaveScreenshot(const std::string &filename, unsigned int width, unsigned int height) {
    auto pixels = MakeScreenshot16(width, height);

    FILE *result = fopen(filename.c_str(), "wb");
    if (result == nullptr) {
        return;
    }

    unsigned int pcx_data_size = width * height * 5;
    uint8_t *pcx_data = new uint8_t[pcx_data_size];
    unsigned int pcx_data_real_size = 0;
    PCX::Encode16(pixels, width, height, pcx_data, pcx_data_size, &pcx_data_real_size);
    fwrite(pcx_data, pcx_data_real_size, 1, result);
    delete[] pcx_data;
    fclose(result);
}

void RenderOpenGL::PackScreenshot(unsigned int width, unsigned int height,
                                  void *out_data, unsigned int data_size,
                                  unsigned int *screenshot_size) {
    auto pixels = MakeScreenshot16(width, height);
    SaveScreenshot("save.pcx", width, height);
    PCX::Encode16(pixels, 150, 112, out_data, 1000000, screenshot_size);
    free(pixels);
}

void RenderOpenGL::SavePCXScreenshot() {
    char file_name[40];
    sprintf(file_name, "screen%0.2i.pcx", ScreenshotFileNumber++ % 100);

    SaveWinnersCertificate(file_name);
}

int RenderOpenGL::GetActorsInViewport(int pDepth) {
    __debugbreak();
    return 0;
}

void RenderOpenGL::BeginLightmaps() {
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    auto effpar03 = assets->GetBitmap("effpar03");
    auto texture = (TextureOpenGL*)effpar03;
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    GL_Check_Errors();
}

void RenderOpenGL::EndLightmaps() {
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, GL_lastboundtex);

    GL_Check_Errors();
}


void RenderOpenGL::BeginLightmaps2() {
    glDisable(GL_CULL_FACE);
    glDepthMask(false);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    static Texture* effpar03 = assets->GetBitmap("effpar03");
    auto texture = (TextureOpenGL*)effpar03;
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    GL_Check_Errors();
}


void RenderOpenGL::EndLightmaps2() {
    glDisable(GL_BLEND);
    glDepthMask(true);
    glEnable(GL_CULL_FACE);
    glBindTexture(GL_TEXTURE_2D, GL_lastboundtex);

    GL_Check_Errors();
}

bool RenderOpenGL::DrawLightmap(struct Lightmap *pLightmap, struct Vec3_float_ *pColorMult, float z_bias) {
    // For outdoor terrain and indoor light (VII)(VII)
    if (pLightmap->NumVertices < 3) {
        log->Warning("Lightmap uNumVertices < 3");
        return false;
    }

    unsigned int uLightmapColorMaskR = (pLightmap->uColorMask >> 16) & 0xFF;
    unsigned int uLightmapColorMaskG = (pLightmap->uColorMask >> 8) & 0xFF;
    unsigned int uLightmapColorMaskB = pLightmap->uColorMask & 0xFF;

    unsigned int uLightmapColorR = floorf(
        uLightmapColorMaskR * pLightmap->fBrightness * pColorMult->x + 0.5f);
    unsigned int uLightmapColorG = floorf(
        uLightmapColorMaskG * pLightmap->fBrightness * pColorMult->y + 0.5f);
    unsigned int uLightmapColorB = floorf(
        uLightmapColorMaskB * pLightmap->fBrightness * pColorMult->z + 0.5f);

    RenderVertexD3D3 pVerticesD3D[64];

    glBegin(GL_TRIANGLE_FAN);

    for (uint i = 0; i < pLightmap->NumVertices; ++i) {
        glColor4f((uLightmapColorR) / 255.0f, (uLightmapColorG) / 255.0f, (uLightmapColorB) / 255.0f, 1.0f);
        glTexCoord2f(pLightmap->pVertices[i].u, pLightmap->pVertices[i].v);
        glVertex3f(pLightmap->pVertices[i].vWorldPosition.x, pLightmap->pVertices[i].vWorldPosition.y, pLightmap->pVertices[i].vWorldPosition.z);
    }

    glEnd();

    drawcalls++;

    GL_Check_Errors();
    return true;
}

void RenderOpenGL::BeginDecals() {
    auto texture = (TextureOpenGL*)assets->GetBitmap("hwsplat04");
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    glDisable(GL_CULL_FACE);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    GL_Check_Errors();
}

void RenderOpenGL::EndDecals() {
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);

    GL_Check_Errors();
}

void RenderOpenGL::DrawDecal(struct Decal *pDecal, float z_bias) {
    // need to add z biasing
    RenderVertexD3D3 pVerticesD3D[64];

    if (pDecal->uNumVertices < 3) {
        log->Warning("Decal has < 3 vertices");
        return;
    }

    float color_mult = pDecal->Fade_by_time();
    if (color_mult == 0.0) return;

    // temp - bloodsplat persistance
    // color_mult = 1;

    glBegin(GL_TRIANGLE_FAN);

    for (uint i = 0; i < (unsigned int)pDecal->uNumVertices; ++i) {
        uint uTint =
            GetActorTintColor(pDecal->DimmingLevel, 0, pDecal->pVertices[i].vWorldViewPosition.x, 0, nullptr);

        uint uTintR = (uTint >> 16) & 0xFF, uTintG = (uTint >> 8) & 0xFF,
            uTintB = uTint & 0xFF;

        uint uDecalColorMultR = (pDecal->uColorMultiplier >> 16) & 0xFF,
            uDecalColorMultG = (pDecal->uColorMultiplier >> 8) & 0xFF,
            uDecalColorMultB = pDecal->uColorMultiplier & 0xFF;

        uint uFinalR =
            floorf(uTintR / 255.0 * color_mult * uDecalColorMultR + 0.0f),
            uFinalG =
            floorf(uTintG / 255.0 * color_mult * uDecalColorMultG + 0.0f),
            uFinalB =
            floorf(uTintB / 255.0 * color_mult * uDecalColorMultB + 0.0f);

        glColor4f((uFinalR) / 255.0f, (uFinalG) / 255.0f, (uFinalB) / 255.0f, 1.0f);
        glTexCoord2f(pDecal->pVertices[i].u, pDecal->pVertices[i].v);
        glVertex3f(pDecal->pVertices[i].vWorldPosition.x, pDecal->pVertices[i].vWorldPosition.y, pDecal->pVertices[i].vWorldPosition.z);

        drawcalls++;
    }
    glEnd();

    GL_Check_Errors();
}

void RenderOpenGL::do_draw_debug_line_d3d(const RenderVertexD3D3 *pLineBegin,
                                          signed int sDiffuseBegin,
                                          const RenderVertexD3D3 *pLineEnd,
                                          signed int sDiffuseEnd,
                                          float z_stuff) {
    __debugbreak();
}
void RenderOpenGL::DrawLines(const RenderVertexD3D3 *vertices,
                             unsigned int num_vertices) {
    __debugbreak();
}
void RenderOpenGL::DrawSpecialEffectsQuad(const RenderVertexD3D3 *vertices,
                                          Texture *texture) {
    __debugbreak();
}

void RenderOpenGL::DrawFromSpriteSheet(Rect *pSrcRect, Point *pTargetPoint, int a3, int blend_mode) {
    // want to draw psrcrect section @ point

    glEnable(GL_TEXTURE_2D);
    float col = (blend_mode == 2) ? 1 : 0.5;

    glColor3f(col, col, col);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto texture = (TextureOpenGL*)pArcomageGame->pSprites;
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    int clipx = this->clip_x;
    int clipy = this->clip_y;
    int clipw = this->clip_w;
    int clipz = this->clip_z;

    int texwidth = texture->GetWidth();
    int texheight = texture->GetHeight();

    int width = pSrcRect->z - pSrcRect->x;
    int height = pSrcRect->w - pSrcRect->y;

    int x = pTargetPoint->x;  // u* window->GetWidth();
    int y = pTargetPoint->y;  // v* window->GetHeight();
    int z = x + width;
    int w = y + height;

    // check bounds
    if (x >= (int)window->GetWidth() || x >= clipz || y >= (int)window->GetHeight() || y >= clipw) return;
    // check for overlap
    if (!(clipx < z && clipz > x && clipy < w && clipw > y)) return;

    int drawx = x;  // std::max(x, clipx);
    int drawy = y;  // std::max(y, clipy);
    int draww = w;  // std::min(w, clipw);
    int drawz = z;  // std::min(z, clipz);

    float depth = 0;

    GLfloat Vertices[] = { (float)drawx, (float)drawy, depth,
        (float)drawz, (float)drawy, depth,
        (float)drawz, (float)draww, depth,
        (float)drawx, (float)draww, depth };

    float texx = pSrcRect->x / float(texwidth);
    float texy = pSrcRect->y / float(texheight);
    float texz = pSrcRect->z / float(texwidth);
    float texw = pSrcRect->w / float(texheight);

    GLfloat TexCoord[] = { texx, texy,
        texz, texy,
        texz, texw,
        texx, texw,
    };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
        0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, TexCoord);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_BLEND);


    GL_Check_Errors();
    return;
}


void RenderOpenGL::PrepareDecorationsRenderList_ODM() {
    unsigned int v6;        // edi@9
    int v7;                 // eax@9
    SpriteFrame *frame;     // eax@9
    unsigned __int16 *v10;  // eax@9
    int v13;                // ecx@9
    char r;                 // ecx@20
    char g;                 // dl@20
    char b_;                // eax@20
    Particle_sw local_0;    // [sp+Ch] [bp-98h]@7
    unsigned __int16 *v37;  // [sp+84h] [bp-20h]@9
    int v38;                // [sp+88h] [bp-1Ch]@9

    for (unsigned int i = 0; i < uNumLevelDecorations; ++i) {
        // LevelDecoration* decor = &pLevelDecorations[i];
        if ((!(pLevelDecorations[i].uFlags & LEVEL_DECORATION_OBELISK_CHEST) ||
            pLevelDecorations[i].IsObeliskChestActive()) &&
            !(pLevelDecorations[i].uFlags & LEVEL_DECORATION_INVISIBLE)) {
            DecorationDesc *decor_desc = pDecorationList->GetDecoration(pLevelDecorations[i].uDecorationDescID);
            if (!(decor_desc->uFlags & 0x80)) {
                if (!(decor_desc->uFlags & 0x22)) {
                    v6 = pMiscTimer->uTotalGameTimeElapsed;
                    v7 = abs(pLevelDecorations[i].vPosition.x +
                        pLevelDecorations[i].vPosition.y);

                    frame = pSpriteFrameTable->GetFrame(decor_desc->uSpriteID,
                        v6 + v7);

                    if (engine->config->seasons_change) {
                        frame = LevelDecorationChangeSeason(decor_desc, v6 + v7, pParty->uCurrentMonth);
                    }

                    if (!frame || frame->texture_name == "null" || frame->hw_sprites[0] == NULL) {
                        continue;
                    }

                    // v8 = pSpriteFrameTable->GetFrame(decor_desc->uSpriteID,
                    // v6 + v7);

                    v10 = (unsigned __int16 *)TrigLUT->Atan2(
                        pLevelDecorations[i].vPosition.x -
                        pCamera3D->vCameraPos.x,
                        pLevelDecorations[i].vPosition.y -
                        pCamera3D->vCameraPos.y);
                    v38 = 0;
                    v13 = ((signed int)(TrigLUT->uIntegerPi +
                        ((signed int)TrigLUT->uIntegerPi >>
                            3) +
                        pLevelDecorations[i].field_10_y_rot -
                        (int64_t)v10) >>
                        8) &
                        7;
                    v37 = (unsigned __int16 *)v13;
                    if (frame->uFlags & 2) v38 = 2;
                    if ((256 << v13) & frame->uFlags) v38 |= 4;
                    if (frame->uFlags & 0x40000) v38 |= 0x40;
                    if (frame->uFlags & 0x20000) v38 |= 0x80;

                    // for light
                    if (frame->uGlowRadius) {
                        r = 255;
                        g = 255;
                        b_ = 255;
                        if (render->config->is_using_colored_lights) {
                            r = decor_desc->uColoredLightRed;
                            g = decor_desc->uColoredLightGreen;
                            b_ = decor_desc->uColoredLightBlue;
                        }
                        pStationaryLightsStack->AddLight(
                            pLevelDecorations[i].vPosition.x,
                            pLevelDecorations[i].vPosition.y,
                            pLevelDecorations[i].vPosition.z +
                            decor_desc->uDecorationHeight / 2,
                            frame->uGlowRadius, r, g, b_, _4E94D0_light_type);
                    }  // for light

                       // v17 = (pLevelDecorations[i].vPosition.x -
                       // pCamera3D->vCameraPos.x) << 16; v40 =
                       // (pLevelDecorations[i].vPosition.y -
                       // pCamera3D->vCameraPos.y) << 16;
                    int party_to_decor_x = pLevelDecorations[i].vPosition.x -
                        pCamera3D->vCameraPos.x;
                    int party_to_decor_y = pLevelDecorations[i].vPosition.y -
                        pCamera3D->vCameraPos.y;
                    int party_to_decor_z = pLevelDecorations[i].vPosition.z -
                        pCamera3D->vCameraPos.z;

                    int view_x = 0;
                    int view_y = 0;
                    int view_z = 0;
                    bool visible = pCamera3D->ViewClip(
                        pLevelDecorations[i].vPosition.x,
                        pLevelDecorations[i].vPosition.y,
                        pLevelDecorations[i].vPosition.z, &view_x, &view_y,
                        &view_z);

                    if (visible) {
                        if (2 * abs(view_x) >= abs(view_y)) {
                            int projected_x = 0;
                            int projected_y = 0;
                            pCamera3D->Project(view_x, view_y, view_z,
                                &projected_x,
                                &projected_y);

                            float _v41 = frame->scale * (pCamera3D->ViewPlaneDist_X) / (view_x);

                            int screen_space_half_width = _v41 * frame->hw_sprites[(int64_t)v37]->uBufferWidth / 2;

                            if (projected_x + screen_space_half_width >=
                                (signed int)pViewport->uViewportTL_X &&
                                projected_x - screen_space_half_width <=
                                (signed int)pViewport->uViewportBR_X) {
                                if (::uNumBillboardsToDraw >= 500) return;
                                ::uNumBillboardsToDraw++;
                                ++uNumDecorationsDrawnThisFrame;

                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .hwsprite = frame->hw_sprites[(int64_t)v37];
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .world_x = pLevelDecorations[i].vPosition.x;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .world_y = pLevelDecorations[i].vPosition.y;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .world_z = pLevelDecorations[i].vPosition.z;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .screen_space_x = projected_x;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .screen_space_y = projected_y;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .screen_space_z = view_x;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .screenspace_projection_factor_x = _v41;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .screenspace_projection_factor_y = _v41;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .uPalette = frame->uPaletteIndex;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .field_1E = v38 | 0x200;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .uIndoorSectorID = 0;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .object_pid = PID(OBJECT_Decoration, i);
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .dimming_level = 0;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .pSpriteFrame = frame;
                                pBillboardRenderList[::uNumBillboardsToDraw - 1]
                                    .sTintColor = 0;
                            }
                        }
                    }
                }
            } else {
                memset(&local_0, 0, sizeof(Particle_sw));
                local_0.type = ParticleType_Bitmap | ParticleType_Rotating |
                    ParticleType_8;
                local_0.uDiffuse = 0xFF3C1E;
                local_0.x = (double)pLevelDecorations[i].vPosition.x;
                local_0.y = (double)pLevelDecorations[i].vPosition.y;
                local_0.z = (double)pLevelDecorations[i].vPosition.z;
                local_0.r = 0.0;
                local_0.g = 0.0;
                local_0.b = 0.0;
                local_0.particle_size = 1.0;
                local_0.timeToLive = (rand() & 0x80) + 128;
                local_0.texture = spell_fx_renderer->effpar01;
                particle_engine->AddParticle(&local_0);
            }
        }
    }
}

/*#pragma pack(push, 1)
typedef struct {
        char  idlength;
        char  colourmaptype;
        char  datatypecode;
        short int colourmaporigin;
        short int colourmaplength;
        char  colourmapdepth;
        short int x_origin;
        short int y_origin;
        short width;
        short height;
        char  bitsperpixel;
        char  imagedescriptor;
} tga;
#pragma pack(pop)

FILE *CreateTga(const char *filename, int image_width, int image_height)
{
        auto f = fopen(filename, "w+b");

        tga tga_header;
        memset(&tga_header, 0, sizeof(tga_header));

        tga_header.colourmaptype = 0;
        tga_header.datatypecode = 2;
        //tga_header.colourmaporigin = 0;
        //tga_header.colourmaplength = image_width * image_height;
        //tga_header.colourmapdepth = 32;
        tga_header.x_origin = 0;
        tga_header.y_origin = 0;
        tga_header.width = image_width;
        tga_header.height = image_height;
        tga_header.bitsperpixel = 32;
        tga_header.imagedescriptor = 32; // top-down
        fwrite(&tga_header, 1, sizeof(tga_header), f);

        return f;
}*/

Texture *RenderOpenGL::CreateTexture_ColorKey(const std::string &name, uint16_t colorkey) {
    return TextureOpenGL::Create(new ColorKey_LOD_Loader(pIcons_LOD, name, colorkey));
}

Texture *RenderOpenGL::CreateTexture_Solid(const std::string &name) {
    return TextureOpenGL::Create(new Image16bit_LOD_Loader(pIcons_LOD, name));
}

Texture *RenderOpenGL::CreateTexture_Alpha(const std::string &name) {
    return TextureOpenGL::Create(new Alpha_LOD_Loader(pIcons_LOD, name));
}

Texture *RenderOpenGL::CreateTexture_PCXFromIconsLOD(const std::string &name) {
    return TextureOpenGL::Create(new PCX_LOD_Compressed_Loader(pIcons_LOD, name));
}

Texture *RenderOpenGL::CreateTexture_PCXFromNewLOD(const std::string &name) {
    return TextureOpenGL::Create(new PCX_LOD_Compressed_Loader(pNew_LOD, name));
}

Texture *RenderOpenGL::CreateTexture_PCXFromFile(const std::string &name) {
    return TextureOpenGL::Create(new PCX_File_Loader(name));
}

Texture *RenderOpenGL::CreateTexture_PCXFromLOD(LOD::File *pLOD, const std::string &name) {
    return TextureOpenGL::Create(new PCX_LOD_Raw_Loader(pLOD, name));
}

Texture *RenderOpenGL::CreateTexture_Blank(unsigned int width, unsigned int height,
    IMAGE_FORMAT format, const void *pixels) {

    return TextureOpenGL::Create(width, height, format, pixels);
}


Texture *RenderOpenGL::CreateTexture(const std::string &name) {
    return TextureOpenGL::Create(new Bitmaps_LOD_Loader(pBitmaps_LOD, name, engine->config->use_hwl_bitmaps));
}

Texture *RenderOpenGL::CreateSprite(const std::string &name, unsigned int palette_id,
                                    /*refactor*/ unsigned int lod_sprite_id) {
    return TextureOpenGL::Create(
        new Sprites_LOD_Loader(pSprites_LOD, palette_id, name, lod_sprite_id));
}

void RenderOpenGL::Update_Texture(Texture *texture) {
    // takes care of endian flip from literals here - hence BGRA

    auto t = (TextureOpenGL *)texture;
    glBindTexture(GL_TEXTURE_2D, t->GetOpenGlTexture());
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->GetWidth(), t->GetHeight(), GL_BGRA, GL_UNSIGNED_BYTE, t->GetPixels(IMAGE_FORMAT_A8R8G8B8));
    glBindTexture(GL_TEXTURE_2D, NULL);

    GL_Check_Errors();
}

void RenderOpenGL::DeleteTexture(Texture *texture) {
    // crash here when assets not loaded as texture

    auto t = (TextureOpenGL *)texture;
    GLuint texid = t->GetOpenGlTexture();
    if (texid != -1) {
        glDeleteTextures(1, &texid);
    }

    GL_Check_Errors();
}

void RenderOpenGL::RemoveTextureFromDevice(Texture* texture) { __debugbreak(); }

bool RenderOpenGL::MoveTextureToDevice(Texture *texture) {
    auto t = (TextureOpenGL *)texture;
    auto native_format = t->GetFormat();
    int gl_format = GL_RGB;
        // native_format == IMAGE_FORMAT_A1R5G5B5 ? GL_RGBA : GL_RGB;

    unsigned __int8 *pixels = nullptr;
    if (native_format == IMAGE_FORMAT_R5G6B5 || native_format == IMAGE_FORMAT_A1R5G5B5 || native_format == IMAGE_FORMAT_A8R8G8B8 || native_format == IMAGE_FORMAT_R8G8B8A8) {
        pixels = (unsigned __int8 *)t->GetPixels(IMAGE_FORMAT_A8R8G8B8);
        // takes care of endian flip from literals here - hence BGRA
        gl_format = GL_BGRA;
    } else {
        log->Warning("Image not loaded!");
    }

    if (pixels) {
        GLuint texid;
        glGenTextures(1, &texid);
        t->SetOpenGlTexture(texid);

        glBindTexture(GL_TEXTURE_2D, texid);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->GetWidth(), t->GetHeight(),
                     0, gl_format, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glBindTexture(GL_TEXTURE_2D, 0);

        GL_Check_Errors();
        return true;
    }
    return false;
}

// TODO(pskelton): move into gl renderer
void _set_3d_projection_matrix() {
    float near_clip = pCamera3D->GetNearClip();
    float far_clip = pCamera3D->GetFarClip();

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    // ogl uses fov in Y - this func has known bug where it misses aspect divsion hence multiplying it to clip distances
    gluPerspective(pCamera3D->fov_y_deg, pCamera3D->aspect, near_clip * pCamera3D->aspect, far_clip * pCamera3D->aspect);

    // build same matrix with glm so we can drop depreciated glu above eventually
    projmat = glm::perspective(glm::radians(pCamera3D->fov_y_deg), pCamera3D->aspect, near_clip * pCamera3D->aspect, far_clip * pCamera3D->aspect);

    GL_Check_Errors();
}

// TODO(pskelton): move into gl renderer
void _set_3d_modelview_matrix() {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glScalef(-1.0f, 1.0f, -1.0f);

    float camera_x = pCamera3D->vCameraPos.x;
    float camera_y = pCamera3D->vCameraPos.y;
    float camera_z = pCamera3D->vCameraPos.z;

    gluLookAt(camera_x, camera_y, camera_z,
              camera_x - cosf(2.0 * pi_double * (float)pCamera3D->sRotationZ / 2048.0f),
              camera_y - sinf(2.0 * pi_double * (float)pCamera3D->sRotationZ / 2048.0f),
              camera_z - tanf(2.0 * pi_double * (float)-pCamera3D->sRotationY / 2048.0f),
              0, 0, 1);

    // build same matrix with glm so we can drop depreciated glu above eventually
    glm::vec3 campos = glm::vec3(camera_x, camera_y, camera_z);
    glm::vec3 eyepos = glm::vec3(camera_x - cosf(2.0 * pi_double * (float)pCamera3D->sRotationZ / 2048.0f),
        camera_y - sinf(2.0 * pi_double * (float)pCamera3D->sRotationZ / 2048.0f),
        camera_z - tanf(2.0 * pi_double * (float)-pCamera3D->sRotationY / 2048.0f));
    glm::vec3 upvec = glm::vec3(0.0, 0.0, 1.0);

    viewmat = glm::lookAtLH(campos, eyepos, upvec);

    GL_Check_Errors();
}

// TODO(pskelton): move into gl renderer
void _set_ortho_projection(bool gameviewport = false) {
    if (!gameviewport) {  // project over entire window
        glViewport(0, 0, window->GetWidth(), window->GetHeight());

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, window->GetWidth(), window->GetHeight(), 0, -1, 1);
    } else {  // project to game viewport
        glViewport(game_viewport_x, window->GetHeight()-game_viewport_w-1, game_viewport_width, game_viewport_height);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(game_viewport_x, game_viewport_z, game_viewport_w, game_viewport_y, 1, -1);  // far = 1 but ogl looks down -z
    }

    GL_Check_Errors();
}

// TODO(pskelton): move into gl renderer
void _set_ortho_modelview() {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    GL_Check_Errors();
}


// ---------------------- terrain -----------------------
const int terrain_block_scale = 512;
const int terrain_height_scale = 32;

// struct for storing vert data for gpu submit
struct GLshaderverts {
    GLfloat x;
    GLfloat y;
    GLfloat z;
    GLfloat u;
    GLfloat v;
    GLfloat texunit;
    GLfloat texturelayer;
    GLfloat normx;
    GLfloat normy;
    GLfloat normz;
    GLfloat attribs;
};

GLshaderverts terrshaderstore[127 * 127 * 6] = {};

void RenderOpenGL::RenderTerrainD3D() {
    // shader version
    // draws entire terrain in one go at the moment
    // textures must all be square and same size
    // terrain is static and verts only submitted once on VAO creation

    // face culling
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);

    // camera matrices
    _set_3d_projection_matrix();
    _set_3d_modelview_matrix();

    // TODO(pskelton): move this to map loading
    // generate array and populate data
    if (terrainVAO == 0) {
        static RenderVertexSoft pTerrainVertices[128 * 128];
        int blockScale = 512;
        int heightScale = 32;

        // generate vertex locations
        for (unsigned int y = 0; y < 128; ++y) {
            for (unsigned int x = 0; x < 128; ++x) {
                pTerrainVertices[y * 128 + x].vWorldPosition.x = (-64 + (signed)x) * blockScale;
                pTerrainVertices[y * 128 + x].vWorldPosition.y = (64 - (signed)y) * blockScale;
                pTerrainVertices[y * 128 + x].vWorldPosition.z = heightScale * pOutdoor->pTerrain.pHeightmap[y * 128 + x];
            }
        }

        // reserve first 7 layers for water tiles in unit 0
        auto wtrtexture = this->hd_water_tile_anim[0];
        terraintexturesizes[0] = wtrtexture->GetWidth();

        for (int buff = 0; buff < 7; buff++) {
            char container_name[64];
            sprintf(container_name, "HDWTR%03u", buff);

            terraintexmap.insert(std::make_pair(container_name, terraintexmap.size()));
            numterraintexloaded[0]++;
        }

        for (int y = 0; y < 127; ++y) {
            for (int x = 0; x < 127; ++x) {
                // map is 127 x 127 squares - each square has two triangles - each tri has 3 verts

                // first find all required textures for terrain and add to map
                auto tile = pOutdoor->DoGetTile(x, y);
                int tileunit = 0;
                int tilelayer = 0;

                // check if tile->name is already in list
                auto mapiter = terraintexmap.find(tile->name);
                if (mapiter != terraintexmap.end()) {
                    // if so, extract unit and layer
                    int unitlayer = mapiter->second;
                    tilelayer = unitlayer & 0xFF;
                    tileunit = (unitlayer & 0xFF00) >> 8;
                } else if (tile->name == "wtrtyl") {
                    // water tile
                    tileunit = 0;
                    tilelayer = 0;
                } else {
                    // else need to add it
                    auto thistexture = assets->GetBitmap(tile->name);
                    int width = thistexture->GetWidth();
                    // check size to see what unit it needs
                    int i;
                    for (i = 0; i < 8; i++) {
                        if (terraintexturesizes[i] == width || terraintexturesizes[i] == 0) break;
                    }
                    if (i == 8) __debugbreak();

                    if (terraintexturesizes[i] == 0) terraintexturesizes[i] = width;

                    tileunit = i;
                    tilelayer = numterraintexloaded[i];

                    // encode unit and layer together
                    int encode = (tileunit << 8) | tilelayer;

                    // intsert into tex map
                    terraintexmap.insert(std::make_pair(tile->name, encode));
                    numterraintexloaded[i]++;
                    if (numterraintexloaded[i] == 256) __debugbreak();
                }

                // next calculate all vertices vertices
                uint norm_idx = pTerrainNormalIndices[(2 * x * 128) + (2 * y) + 2 /*+ 1*/];  // 2 is top tri // 3 is bottom
                uint bottnormidx = pTerrainNormalIndices[(2 * x * 128) + (2 * y) + 3];
                assert(norm_idx < uNumTerrainNormals);
                assert(bottnormidx < uNumTerrainNormals);
                Vec3_float_ *norm = &pTerrainNormals[norm_idx];
                Vec3_float_ *norm2 = &pTerrainNormals[bottnormidx];

                // calc each vertex
                // [0] - x,y        n1
                terrshaderstore[6 * (x + (127 * y))].x = pTerrainVertices[y * 128 + x].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y))].y = pTerrainVertices[y * 128 + x].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y))].z = pTerrainVertices[y * 128 + x].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y))].u = 0;
                terrshaderstore[6 * (x + (127 * y))].v = 0;
                terrshaderstore[6 * (x + (127 * y))].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y))].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y))].normx = norm->x;
                terrshaderstore[6 * (x + (127 * y))].normy = norm->y;
                terrshaderstore[6 * (x + (127 * y))].normz = norm->z;
                terrshaderstore[6 * (x + (127 * y))].attribs = 0;

                // [1] - x+1,y+1    n1
                terrshaderstore[6 * (x + (127 * y)) + 1].x = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y)) + 1].y = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y)) + 1].z = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y)) + 1].u = 1;
                terrshaderstore[6 * (x + (127 * y)) + 1].v = 1;
                terrshaderstore[6 * (x + (127 * y)) + 1].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y)) + 1].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y)) + 1].normx = norm->x;
                terrshaderstore[6 * (x + (127 * y)) + 1].normy = norm->y;
                terrshaderstore[6 * (x + (127 * y)) + 1].normz = norm->z;
                terrshaderstore[6 * (x + (127 * y)) + 1].attribs = 0;

                // [2] - x+1,y      n1
                terrshaderstore[6 * (x + (127 * y)) + 2].x = pTerrainVertices[y * 128 + x + 1].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y)) + 2].y = pTerrainVertices[y * 128 + x + 1].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y)) + 2].z = pTerrainVertices[y * 128 + x + 1].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y)) + 2].u = 1;
                terrshaderstore[6 * (x + (127 * y)) + 2].v = 0;
                terrshaderstore[6 * (x + (127 * y)) + 2].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y)) + 2].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y)) + 2].normx = norm->x;
                terrshaderstore[6 * (x + (127 * y)) + 2].normy = norm->y;
                terrshaderstore[6 * (x + (127 * y)) + 2].normz = norm->z;
                terrshaderstore[6 * (x + (127 * y)) + 2].attribs = 0;

                // [3] - x,y        n2
                terrshaderstore[6 * (x + (127 * y)) + 3].x = pTerrainVertices[y * 128 + x].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y)) + 3].y = pTerrainVertices[y * 128 + x].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y)) + 3].z = pTerrainVertices[y * 128 + x].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y)) + 3].u = 0;
                terrshaderstore[6 * (x + (127 * y)) + 3].v = 0;
                terrshaderstore[6 * (x + (127 * y)) + 3].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y)) + 3].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y)) + 3].normx = norm2->x;
                terrshaderstore[6 * (x + (127 * y)) + 3].normy = norm2->y;
                terrshaderstore[6 * (x + (127 * y)) + 3].normz = norm2->z;
                terrshaderstore[6 * (x + (127 * y)) + 3].attribs = 0;

                // [4] - x,y+1      n2
                terrshaderstore[6 * (x + (127 * y)) + 4].x = pTerrainVertices[(y + 1) * 128 + x].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y)) + 4].y = pTerrainVertices[(y + 1) * 128 + x].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y)) + 4].z = pTerrainVertices[(y + 1) * 128 + x].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y)) + 4].u = 0;
                terrshaderstore[6 * (x + (127 * y)) + 4].v = 1;
                terrshaderstore[6 * (x + (127 * y)) + 4].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y)) + 4].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y)) + 4].normx = norm2->x;
                terrshaderstore[6 * (x + (127 * y)) + 4].normy = norm2->y;
                terrshaderstore[6 * (x + (127 * y)) + 4].normz = norm2->z;
                terrshaderstore[6 * (x + (127 * y)) + 4].attribs = 0;

                // [5] - x+1,y+1    n2
                terrshaderstore[6 * (x + (127 * y)) + 5].x = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.x;
                terrshaderstore[6 * (x + (127 * y)) + 5].y = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.y;
                terrshaderstore[6 * (x + (127 * y)) + 5].z = pTerrainVertices[(y + 1) * 128 + x + 1].vWorldPosition.z;
                terrshaderstore[6 * (x + (127 * y)) + 5].u = 1;
                terrshaderstore[6 * (x + (127 * y)) + 5].v = 1;
                terrshaderstore[6 * (x + (127 * y)) + 5].texunit = tileunit;
                terrshaderstore[6 * (x + (127 * y)) + 5].texturelayer = tilelayer;
                terrshaderstore[6 * (x + (127 * y)) + 5].normx = norm2->x;
                terrshaderstore[6 * (x + (127 * y)) + 5].normy = norm2->y;
                terrshaderstore[6 * (x + (127 * y)) + 5].normz = norm2->z;
                terrshaderstore[6 * (x + (127 * y)) + 5].attribs = 0;
            }
        }

        // generate VAO
        glGenVertexArrays(1, &terrainVAO);
        glGenBuffers(1, &terrainVBO);

        glBindVertexArray(terrainVAO);
        glBindBuffer(GL_ARRAY_BUFFER, terrainVBO);

        // submit vert data
        glBufferData(GL_ARRAY_BUFFER, sizeof(terrshaderstore), terrshaderstore, GL_STATIC_DRAW);
        // submit data layout
        // position attribute
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (11 * sizeof(GLfloat)), (void *)0);
        glEnableVertexAttribArray(0);
        // tex uv attribute
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, (11 * sizeof(GLfloat)), (void *)(3 * sizeof(GLfloat)));
        glEnableVertexAttribArray(1);
        // tex unit attribute
        // tex array layer attribute
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (11 * sizeof(GLfloat)), (void *)(5 * sizeof(GLfloat)));
        glEnableVertexAttribArray(2);
        // normals
        glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, (11 * sizeof(GLfloat)), (void *)(7 * sizeof(GLfloat)));
        glEnableVertexAttribArray(3);
        // attribs - not used here yet
        glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, (11 * sizeof(GLfloat)), (void *)(10 * sizeof(GLfloat)));
        glEnableVertexAttribArray(4);

        GL_Check_Errors();

        // texture set up - load in all previously found
        for (int unit = 0; unit < 8; unit++) {
            assert(numterraintexloaded[unit] <= 256);
            // skip if textures are empty
            if (numterraintexloaded[unit] == 0) continue;

            glGenTextures(1, &terraintextures[unit]);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terraintextures[unit]);

            // create blank memory for later texture submission
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, terraintexturesizes[unit], terraintexturesizes[unit], numterraintexloaded[unit], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

            // loop through texture map
            std::map<std::string, int>::iterator it = terraintexmap.begin();
            while (it != terraintexmap.end()) {
                int comb = it->second;
                int tlayer = comb & 0xFF;
                int tunit = (comb & 0xFF00) >> 8;

                if (tunit == unit) {
                    // get texture
                    auto texture = assets->GetBitmap(it->first);
                    // send texture data to gpu
                    glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                        0,
                        0, 0, tlayer,
                        terraintexturesizes[unit], terraintexturesizes[unit], 1,
                        GL_RGBA,
                        GL_UNSIGNED_BYTE,
                        texture->GetPixels(IMAGE_FORMAT_R8G8B8A8));
                }

                it++;
            }

            // last texture setups
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

            GL_Check_Errors();
        }
    }

/////////////////////////////////////////////////////
    // actual drawing

    // terrain debug
    if (engine->config->debug_terrain)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);


    // load texture arrays in - we only use unit 0 for water and unit 1 for tiles for time being
    for (int unit = 0; unit < 8; unit++) {
        // skip if textures are empty
        if (numterraintexloaded[unit] > 0) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, terraintextures[unit]);
        }
        GL_Check_Errors();
    }

    // load terrain verts
    glBindVertexArray(terrainVAO);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glEnableVertexAttribArray(3);
    glEnableVertexAttribArray(4);

    // use the terrain shader
    glUseProgram(terrainshader.ID);

    GL_Check_Errors();

    // set projection matrix
    glUniformMatrix4fv(glGetUniformLocation(terrainshader.ID, "projection"), 1, GL_FALSE, &projmat[0][0]);
    // set view matrix
    glUniformMatrix4fv(glGetUniformLocation(terrainshader.ID, "view"), 1, GL_FALSE, &viewmat[0][0]);
    // set animated water frame
    glUniform1i(glGetUniformLocation(terrainshader.ID, "waterframe"), GLint(this->hd_water_current_frame));
    // set texture unit location
    glUniform1i(glGetUniformLocation(terrainshader.ID, "textureArray0"), GLint(0));
    glUniform1i(glGetUniformLocation(terrainshader.ID, "textureArray1"), GLint(1));

    GLfloat camera[3];
    camera[0] = (float)(pParty->vPosition.x - pParty->y_rotation_granularity * cosf(2 * pi_double * pParty->sRotationZ / 2048.0));
    camera[1] = (float)(pParty->vPosition.y - pParty->y_rotation_granularity * sinf(2 * pi_double * pParty->sRotationZ / 2048.0));
    camera[2] = (float)(pParty->vPosition.z + pParty->sEyelevel);
    glUniform3fv(glGetUniformLocation(terrainshader.ID, "CameraPos"), 1, &camera[0]);


    // sun lighting stuff
    float ambient = pParty->uCurrentMinute + pParty->uCurrentHour * 60.0;  // 0 - > 1439
    ambient = 0.15 + (sinf(((ambient - 360.0) * 2 * pi_double) / 1440) + 1) * 0.27;
    float diffuseon = pWeather->bNight ? 0 : 1;

    glUniform3fv(glGetUniformLocation(terrainshader.ID, "sun.direction"), 1, &pOutdoor->vSunlight[0]);
    glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.ambient"), ambient, ambient, ambient);
    glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.diffuse"), diffuseon * (ambient + 0.3), diffuseon * (ambient + 0.3), diffuseon * (ambient + 0.3));
    glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.specular"), diffuseon * 1.0, diffuseon * 0.8, 0.0);

    // red colouring
    if (pParty->armageddon_timer) {
        glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.ambient"), 1.0, 0, 0);
        glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.diffuse"), 1.0, 0, 0);
        glUniform3f(glGetUniformLocation(terrainshader.ID, "sun.specular"), 0, 0, 0);
    }

    // torchlight - pointlight 1 is always party glow
    float torchradius = 0;
    if (!diffuseon) {
        int rangemult = 1;
        if (pParty->pPartyBuffs[PARTY_BUFF_TORCHLIGHT].Active())
            rangemult = pParty->pPartyBuffs[PARTY_BUFF_TORCHLIGHT].uPower;
        torchradius = float(rangemult) * 1024.0;
    }

    glUniform3fv(glGetUniformLocation(terrainshader.ID, "fspointlights[0].position"), 1, &camera[0]);
    glUniform3f(glGetUniformLocation(terrainshader.ID, "fspointlights[0].ambient"), 0.85, 0.85, 0.85);  // background
    glUniform3f(glGetUniformLocation(terrainshader.ID, "fspointlights[0].diffuse"), 0.85, 0.85, 0.85);  // direct
    glUniform3f(glGetUniformLocation(terrainshader.ID, "fspointlights[0].specular"), 0, 0, 1);          // for "shinyness"
    glUniform1f(glGetUniformLocation(terrainshader.ID, "fspointlights[0].radius"), torchradius);


    // rest of lights stacking
    GLuint num_lights = 1;
    for (int i = 0; i < pMobileLightsStack->uNumLightsActive; ++i) {
        // maximum 20 lights sent to shader at the moment
        // TODO(pskelton): make this configurable - also lights should be sorted by distance so nearest are used first
        if (num_lights >= 20) break;

        std::string slotnum = std::to_string(num_lights);
        auto test = pMobileLightsStack->pLights[i];

        float x = pMobileLightsStack->pLights[i].vPosition.x;
        float y = pMobileLightsStack->pLights[i].vPosition.y;
        float z = pMobileLightsStack->pLights[i].vPosition.z;

        float r = pMobileLightsStack->pLights[i].uLightColorR / 255.0;
        float g = pMobileLightsStack->pLights[i].uLightColorG / 255.0;
        float b = pMobileLightsStack->pLights[i].uLightColorB / 255.0;

        float lightrad = pMobileLightsStack->pLights[i].uRadius;

        glUniform1f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].type").c_str()), 2.0);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].position").c_str()), x, y, z);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].ambient").c_str()), r, g, b);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].diffuse").c_str()), r, g, b);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].specular").c_str()), r, g, b);
        glUniform1f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].radius").c_str()), lightrad);

        num_lights++;
    }

    for (int i = 0; i < pStationaryLightsStack->uNumLightsActive; ++i) {
        // maximum 20 lights sent to shader at the moment
        // TODO(pskelton): make this configurable - also lights should be sorted by distance so nearest are used first
        if (num_lights >= 20) break;

        std::string slotnum = std::to_string(num_lights);
        auto test = pStationaryLightsStack->pLights[i];

        float x = test.vPosition.x;
        float y = test.vPosition.y;
        float z = test.vPosition.z;

        float r = test.uLightColorR / 255.0;
        float g = test.uLightColorG / 255.0;
        float b = test.uLightColorB / 255.0;

        float lightrad = test.uRadius;

        glUniform1f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].type").c_str()), 1.0);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].position").c_str()), x, y, z);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].ambient").c_str()), r, g, b);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].diffuse").c_str()), r, g, b);
        glUniform3f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].specular").c_str()), r, g, b);
        glUniform1f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].radius").c_str()), lightrad);

        num_lights++;
    }

    // blank the rest of the lights
    for (int blank = num_lights; blank < 20; blank++) {
        std::string slotnum = std::to_string(blank);
        glUniform1f(glGetUniformLocation(terrainshader.ID, ("fspointlights[" + slotnum + "].type").c_str()), 0.0);
    }

    GL_Check_Errors();

    // actually draw the whole terrain
    glDrawArrays(GL_TRIANGLES, 0, (127 * 127 * 6));
    drawcalls++;
    GL_Check_Errors();

    // unload
    glUseProgram(0);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDisableVertexAttribArray(2);
    glDisableVertexAttribArray(3);
    glDisableVertexAttribArray(4);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, NULL);

    //end terrain debug
    if (engine->config->debug_terrain)
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    GL_Check_Errors();

    // stack new decals onto terrain faces ////////////////////////////////////////////////
    // TODO(pskelton): clean up
    if (!decal_builder->bloodsplat_container->uNumBloodsplats) return;
    unsigned int NumBloodsplats = decal_builder->bloodsplat_container->uNumBloodsplats;

    // loop over blood to lay
    for (uint i = 0; i < NumBloodsplats; ++i) {
        // approx location of bloodsplat
        int splatx = decal_builder->bloodsplat_container->pBloodsplats_to_apply[i].x;
        int splaty = decal_builder->bloodsplat_container->pBloodsplats_to_apply[i].y;
        int splatz = decal_builder->bloodsplat_container->pBloodsplats_to_apply[i].z;
        int testx = WorldPosToGridCellX(splatx);
        int testy = WorldPosToGridCellY(splaty);
        // use terrain squares in block surrounding to try and stack faces

        for (int loopy = (testy - 1); loopy < (testy + 1); ++loopy) {
            for (int loopx = (testx - 1); loopx < (testx + 1); ++loopx) {
                if (loopy < 0) continue;
                if (loopy > 126) continue;
                if (loopx < 0) continue;
                if (loopx > 126) continue;

                struct Polygon *pTilePolygon = &array_77EC08[pODMRenderParams->uNumPolygons];
                pTilePolygon->flags = pOutdoor->GetSomeOtherTileInfo(loopx, loopy);

                uint norm_idx = pTerrainNormalIndices[(2 * loopx * 128) + (2 * loopy) + 2];  // 2 is top tri // 3 is bottom
                uint bottnormidx = pTerrainNormalIndices[(2 * loopx * 128) + (2 * loopy) + 3];
                assert(norm_idx < uNumTerrainNormals);
                assert(bottnormidx < uNumTerrainNormals);
                Vec3_float_ *norm = &pTerrainNormals[norm_idx];
                Vec3_float_ *norm2 = &pTerrainNormals[bottnormidx];

                float _f1 = norm->x * pOutdoor->vSunlight.x + norm->y * pOutdoor->vSunlight.y + norm->z * pOutdoor->vSunlight.z;
                pTilePolygon->dimming_level = 20.0 - floorf(20.0 * _f1 + 0.5f);
                pTilePolygon->dimming_level = std::clamp((int)pTilePolygon->dimming_level, 0, 31);

                float Light_tile_dist = 0.0;

                int blockScale = 512;
                int heightScale = 32;

                static stru154 static_sub_0048034E_stru_154;

                // top tri
                // x, y
                VertexRenderList[0].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy))].x;
                VertexRenderList[0].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy))].y;
                VertexRenderList[0].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy))].z;
                // x + 1, y + 1
                VertexRenderList[1].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy)) + 1].x;
                VertexRenderList[1].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy)) + 1].y;
                VertexRenderList[1].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy)) + 1].z;
                // x + 1, y
                VertexRenderList[2].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy)) + 2].x;
                VertexRenderList[2].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy)) + 2].y;
                VertexRenderList[2].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy)) + 2].z;

                decal_builder->ApplyBloodSplatToTerrain(pTilePolygon, norm, &Light_tile_dist, VertexRenderList, 3, 1);
                static_sub_0048034E_stru_154.ClassifyPolygon(norm, Light_tile_dist);
                if (decal_builder->uNumSplatsThisFace > 0)
                    decal_builder->BuildAndApplyDecals(31 - pTilePolygon->dimming_level, 4, &static_sub_0048034E_stru_154, 3, VertexRenderList, 0/**(float*)&uClipFlag*/, -1);

                //bottom tri
                float _f = norm2->x * pOutdoor->vSunlight.x + norm2->y * pOutdoor->vSunlight.y + norm2->z * pOutdoor->vSunlight.z;
                pTilePolygon->dimming_level = 20.0 - floorf(20.0 * _f + 0.5f);
                pTilePolygon->dimming_level = std::clamp((int)pTilePolygon->dimming_level, 0, 31);

                // x, y
                VertexRenderList[0].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy)) + 3].x;
                VertexRenderList[0].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy)) + 3].y;
                VertexRenderList[0].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy)) + 3].z;
                // x, y + 1
                VertexRenderList[1].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy)) + 4].x;
                VertexRenderList[1].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy)) + 4].y;
                VertexRenderList[1].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy)) + 4].z;
                // x + 1, y + 1
                VertexRenderList[2].vWorldPosition.x = terrshaderstore[6 * (loopx + (127 * loopy)) + 5].x;
                VertexRenderList[2].vWorldPosition.y = terrshaderstore[6 * (loopx + (127 * loopy)) + 5].y;
                VertexRenderList[2].vWorldPosition.z = terrshaderstore[6 * (loopx + (127 * loopy)) + 5].z;

                decal_builder->ApplyBloodSplatToTerrain(pTilePolygon, norm2, &Light_tile_dist, VertexRenderList, 3, 0);
                static_sub_0048034E_stru_154.ClassifyPolygon(norm2, Light_tile_dist);
                if (decal_builder->uNumSplatsThisFace > 0)
                    decal_builder->BuildAndApplyDecals(31 - pTilePolygon->dimming_level, 4, &static_sub_0048034E_stru_154, 3, VertexRenderList, 0/**(float*)&uClipFlag_2*/, -1);
            }
        }
    }

    // end of new system test
    GL_Check_Errors();
    return;

    // end shder version
}

// this is now obselete with shader terrain drawing
void RenderOpenGL::DrawTerrainPolygon(struct Polygon *poly, bool transparent, bool clampAtTextureBorders) { return; }

void RenderOpenGL::DrawOutdoorSkyD3D() {
    double rot_to_rads = ((2 * pi_double) / 2048);

    // lowers clouds as party goes up
    float  horizon_height_offset = ((double)(pCamera3D->ViewPlaneDist_X * pCamera3D->vCameraPos.z)
        / ((double)pCamera3D->ViewPlaneDist_X + pCamera3D->GetFarClip())
        + (double)(pViewport->uScreenCenterY));

    float depth_to_far_clip = cos((double)pCamera3D->sRotationY * rot_to_rads) * pCamera3D->GetFarClip();
    float height_to_far_clip = sin((double)pCamera3D->sRotationY * rot_to_rads) * pCamera3D->GetFarClip();

    float bot_y_proj = ((double)(pViewport->uScreenCenterY) -
        (double)pCamera3D->ViewPlaneDist_X /
        (depth_to_far_clip + 0.0000001) *
        (height_to_far_clip - (double)pCamera3D->vCameraPos.z));

    struct Polygon pSkyPolygon;
    pSkyPolygon.texture = nullptr;
    pSkyPolygon.ptr_38 = &SkyBillboard;


    // if ( pParty->uCurrentHour > 20 || pParty->uCurrentHour < 5 )
    // pSkyPolygon.uTileBitmapID = pOutdoor->New_SKY_NIGHT_ID;
    // else
    // pSkyPolygon.uTileBitmapID = pOutdoor->sSky_TextureID;//179(original 166)
    // pSkyPolygon.pTexture = (Texture_MM7 *)(pSkyPolygon.uTileBitmapID != -1 ?
    // (int)&pBitmaps_LOD->pTextures[pSkyPolygon.uTileBitmapID] : 0);

    if (!pOutdoor->sky_texture)
        pOutdoor->sky_texture = assets->GetBitmap("plansky3");

    pSkyPolygon.texture = pOutdoor->sky_texture;
    if (pSkyPolygon.texture) {
        pSkyPolygon.dimming_level = (uCurrentlyLoadedLevelType == LEVEL_Outdoor)? 31 : 0;
        pSkyPolygon.uNumVertices = 4;

        // centering(центруем)-----------------------------------------------------------------
        // plane of sky polygon rotation vector - pitch rotation around y
        float v18x = -sin((-pCamera3D->sRotationY + 16) * rot_to_rads);
        float v18y = 0;
        float v18z = -cos((pCamera3D->sRotationY + 16) * rot_to_rads);

        // sky wiew position(положение неба на
        // экране)------------------------------------------
        //                X
        // 0._____________________________.3
        //  |8,8                    468,8 |
        //  |                             |
        //  |                             |
        // Y|                             |
        //  |                             |
        //  |8,351                468,351 |
        // 1._____________________________.2
        //
        VertexRenderList[0].vWorldViewProjX = (double)(signed int)pViewport->uViewportTL_X;  // 8
        VertexRenderList[0].vWorldViewProjY = (double)(signed int)pViewport->uViewportTL_Y;  // 8

        VertexRenderList[1].vWorldViewProjX = (double)(signed int)pViewport->uViewportTL_X;   // 8
        VertexRenderList[1].vWorldViewProjY = (double)bot_y_proj + 1;  // 247

        VertexRenderList[2].vWorldViewProjX = (double)(signed int)pViewport->uViewportBR_X;   // 468
        VertexRenderList[2].vWorldViewProjY = (double)bot_y_proj + 1;  // 247

        VertexRenderList[3].vWorldViewProjX = (double)(signed int)pViewport->uViewportBR_X;  // 468
        VertexRenderList[3].vWorldViewProjY = (double)(signed int)pViewport->uViewportTL_Y;  // 8

        float widthperpixel = 1 / pCamera3D->ViewPlaneDist_X;

        for (uint i = 0; i < pSkyPolygon.uNumVertices; ++i) {
            // outbound screen X dist
            float x_dist = widthperpixel * (pViewport->uScreenCenterX - VertexRenderList[i].vWorldViewProjX);
            // outbound screen y dist
            float y_dist = widthperpixel * (horizon_height_offset - VertexRenderList[i].vWorldViewProjY);

            // rotate vectors to cam facing
            float skyfinalleft = (pSkyPolygon.ptr_38->CamVecLeft_X * x_dist) + (pSkyPolygon.ptr_38->CamVecLeft_Z * y_dist) + pSkyPolygon.ptr_38->CamVecLeft_Y;
            float skyfinalfront = (pSkyPolygon.ptr_38->CamVecFront_X * x_dist) + (pSkyPolygon.ptr_38->CamVecFront_Z * y_dist) + pSkyPolygon.ptr_38->CamVecFront_Y;

            // pitch rotate sky to get top
            float top_y_proj = v18x + v18y + v18z * y_dist;
            if (top_y_proj > 0) top_y_proj = -0.0000001;

            float worldviewdepth = -64.0 / top_y_proj;
            if (worldviewdepth < 0) worldviewdepth = pCamera3D->GetFarClip();

            // offset tex coords
            float texoffset_U = (float(pMiscTimer->uTotalGameTimeElapsed) / 128.0) + ((skyfinalleft * worldviewdepth));
            VertexRenderList[i].u = texoffset_U / ((float)pSkyPolygon.texture->GetWidth());
            float texoffset_V = (float(pMiscTimer->uTotalGameTimeElapsed) / 128.0) + ((skyfinalfront * worldviewdepth));
            VertexRenderList[i].v = texoffset_V / ((float)pSkyPolygon.texture->GetHeight());

            VertexRenderList[i].vWorldViewPosition.x = pCamera3D->GetFarClip();

            // this basically acts as texture perspective correction
            VertexRenderList[i]._rhw = /*1.0 /*/ (double)(worldviewdepth);
        }

        _set_ortho_projection(1);
        _set_ortho_modelview();
        DrawOutdoorSkyPolygon(&pSkyPolygon);
    }
}

//----- (004A2DA3) --------------------------------------------------------
void RenderOpenGL::DrawOutdoorSkyPolygon(struct Polygon *pSkyPolygon) {
    auto texture = (TextureOpenGL *)pSkyPolygon->texture;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBegin(GL_TRIANGLE_FAN);
    {
        for (int i = 0; i < pSkyPolygon->uNumVertices; ++i) {
            unsigned int diffuse = ::GetActorTintColor(
                pSkyPolygon->dimming_level, 0, VertexRenderList[i].vWorldViewPosition.x, 1, 0);

            glColor4f(((diffuse >> 16) & 0xFF) / 255.0f,
                      ((diffuse >> 8) & 0xFF) / 255.0f,
                      (diffuse & 0xFF) / 255.0f, 1.0f);

            glTexCoord2f(VertexRenderList[i].u, VertexRenderList[i].v);

            // shoe horn in perspective correction
            glVertex4f(VertexRenderList[i].vWorldViewProjX * VertexRenderList[i]._rhw,
                VertexRenderList[i].vWorldViewProjY * VertexRenderList[i]._rhw,
                1.0 * VertexRenderList[i]._rhw, VertexRenderList[i]._rhw);
        }
    }
    drawcalls++;
    glEnd();

    GL_Check_Errors();
}

void RenderOpenGL::DrawBillboards_And_MaybeRenderSpecialEffects_And_EndScene() {
    engine->draw_debug_outlines();
    this->DoRenderBillboards_D3D();
    spell_fx_renderer->RenderSpecialEffects();
}

//----- (004A1C1E) --------------------------------------------------------
void RenderOpenGL::DoRenderBillboards_D3D() {
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);  // in theory billboards all sorted by depth so dont cull by depth test
    glDisable(GL_CULL_FACE);  // some quads are reversed to reuse sprites opposite hand
    glEnable(GL_TEXTURE_2D);

    _set_ortho_projection(1);
    _set_ortho_modelview();

    for (int i = uNumBillboardsToDraw - 1; i >= 0; --i) {
        if (pBillboardRenderListD3D[i].opacity != RenderBillboardD3D::NoBlend) {
            SetBillboardBlendOptions(pBillboardRenderListD3D[i].opacity);
        }

        float gltexid = 0;
        if (pBillboardRenderListD3D[i].texture) {
            auto texture = (TextureOpenGL *)pBillboardRenderListD3D[i].texture;
            gltexid = texture->GetOpenGlTexture();
        }

        glBindTexture(GL_TEXTURE_2D, gltexid);

        glBegin(GL_TRIANGLE_FAN);
        {
            auto billboard = &pBillboardRenderListD3D[i];
            auto b = &pBillboardRenderList[i];

            for (unsigned int j = 0; j < billboard->uNumVertices; ++j) {
                glColor4f(
                    ((billboard->pQuads[j].diffuse >> 16) & 0xFF) / 255.0f,
                    ((billboard->pQuads[j].diffuse >> 8) & 0xFF) / 255.0f,
                    ((billboard->pQuads[j].diffuse >> 0) & 0xFF) / 255.0f,
                    1.0f);

                glTexCoord2f(billboard->pQuads[j].texcoord.x,
                             billboard->pQuads[j].texcoord.y);

                float oneoz = 1. / billboard->screen_space_z;
                float oneon = 1. / (pCamera3D->GetNearClip() * pCamera3D->aspect * 2);
                float oneof = 1. / (pCamera3D->GetFarClip() * pCamera3D->aspect);

                glVertex3f(
                    billboard->pQuads[j].pos.x,
                    billboard->pQuads[j].pos.y,
                    (oneoz - oneon)/(oneof - oneon));  // depth is  non linear  proportional to reciprocal of distance
            }
        }
        drawcalls++;
        glEnd();
    }

    // uNumBillboardsToDraw = 0;


    if (config->is_using_fog) {
        SetUsingFog(false);
        glEnable(GL_FOG);
        glFogi(GL_FOG_MODE, GL_EXP);

        GLfloat fog_color[] = {((GetLevelFogColor() >> 16) & 0xFF) / 255.0f,
                               ((GetLevelFogColor() >> 8) & 0xFF) / 255.0f,
                               ((GetLevelFogColor() >> 0) & 0xFF) / 255.0f,
                               1.0f};
        glFogfv(GL_FOG_COLOR, fog_color);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);

    GL_Check_Errors();
}

//----- (004A1DA8) --------------------------------------------------------
void RenderOpenGL::SetBillboardBlendOptions(RenderBillboardD3D::OpacityType a1) {
    switch (a1) {
        case RenderBillboardD3D::Transparent: {
            if (config->is_using_fog) {
                SetUsingFog(false);
                glEnable(GL_FOG);
                glFogi(GL_FOG_MODE, GL_EXP);

                GLfloat fog_color[] = {
                    ((GetLevelFogColor() >> 16) & 0xFF) / 255.0f,
                    ((GetLevelFogColor() >> 8) & 0xFF) / 255.0f,
                    ((GetLevelFogColor() >> 0) & 0xFF) / 255.0f, 1.0f};
                glFogfv(GL_FOG_COLOR, fog_color);
            }

            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        } break;

        case RenderBillboardD3D::Opaque_1:
        case RenderBillboardD3D::Opaque_2:
        case RenderBillboardD3D::Opaque_3: {
            if (config->is_using_specular) {
                if (!config->is_using_fog) {
                    SetUsingFog(true);
                    glDisable(GL_FOG);
                }
            }

            glBlendFunc(GL_ONE, GL_ONE);  // zero
        } break;

        default:
            log->Warning(
                "SetBillboardBlendOptions: invalid opacity type (%u)", a1);
            assert(false);
            break;
    }

    GL_Check_Errors();
}

void RenderOpenGL::SetUIClipRect(unsigned int x, unsigned int y, unsigned int z,
                                 unsigned int w) {
    this->clip_x = x;
    this->clip_y = y;
    this->clip_z = z;
    this->clip_w = w;
    glScissor(x, this->window->GetHeight() -w, z-x, w-y);  // invert glscissor co-ords 0,0 is BL

    GL_Check_Errors();
}

void RenderOpenGL::ResetUIClipRect() {
    this->SetUIClipRect(0, 0, this->window->GetWidth(), this->window->GetHeight());
}

void RenderOpenGL::PresentBlackScreen() {
    BeginScene();
    ClearBlack();
    EndScene();
    Present();
}

void RenderOpenGL::BeginScene() {
    // Setup for 2D

    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    _set_ortho_projection();
    _set_ortho_modelview();

    GL_Check_Errors();
}

void RenderOpenGL::EndScene() {
    // blank in d3d
}



void RenderOpenGL::DrawTextureAlphaNew(float u, float v, Image *img) {
    DrawTextureNew(u, v, img);
    return;
}

void RenderOpenGL::DrawTextureNew(float u, float v, Image *tex, uint32_t colourmask) {
    if (!tex) __debugbreak();

    TextureOpenGL *texture = dynamic_cast<TextureOpenGL *>(tex);
    if (!texture) {
        __debugbreak();
        return;
    }

    float r = ((colourmask >> 16) & 0xFF) / 255.0f;
    float g = ((colourmask >> 8) & 0xFF) / 255.0f;
    float b = ((colourmask >> 0) & 0xFF) / 255.0f;

    glEnable(GL_TEXTURE_2D);
    glColor3f(r, g, b);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    int clipx = this->clip_x;
    int clipy = this->clip_y;
    int clipw = this->clip_w;
    int clipz = this->clip_z;

    int width = tex->GetWidth();
    int height = tex->GetHeight();

    int x = u * window->GetWidth();
    int y = v * window->GetHeight();
    int z = x + width;
    int w = y + height;

    // check bounds
    if (x >= (int)window->GetWidth() || x >= clipz || y >= (int)window->GetHeight() || y >= clipw) return;
    // check for overlap
    if ((clipx < z && clipz > x && clipy > w && clipw < y)) return;

    int drawx = std::max(x, clipx);
    int drawy = std::max(y, clipy);
    int draww = std::min(w, clipw);
    int drawz = std::min(z, clipz);
    if (drawz <= drawx || draww <= drawy) return;

    float depth = 0;

    GLfloat Vertices[] = { (float)drawx, (float)drawy, depth,
        (float)drawz, (float)drawy, depth,
        (float)drawz, (float)draww, depth,
        (float)drawx, (float)draww, depth };

    float texx = (drawx - x) / float(width);
    float texy = (drawy - y) / float(height);
    float texz = (drawz - x) / float(width);
    float texw = (draww - y) / float(height);

    GLfloat TexCoord[] = { texx, texy,
        texz, texy,
        texz, texw,
        texx, texw,
    };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
       0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, TexCoord);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_BLEND);

    GL_Check_Errors();
}

void RenderOpenGL::DrawTextureCustomHeight(float u, float v, class Image *img, int custom_height) {
    if (!img) __debugbreak();

    TextureOpenGL *texture = dynamic_cast<TextureOpenGL *>(img);
    if (!texture) {
        __debugbreak();
        return;
    }

    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;

    glEnable(GL_TEXTURE_2D);
    glColor3f(r, g, b);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    int clipx = this->clip_x;
    int clipy = this->clip_y;
    int clipw = this->clip_w;
    int clipz = this->clip_z;

    int width = img->GetWidth();
    int height = img->GetHeight();

    int x = u * window->GetWidth();
    int y = v * window->GetHeight();
    int z = x + width;
    int w = y + custom_height;

    // check bounds
    if (x >= (int)window->GetWidth() || x >= clipz || y >= (int)window->GetHeight() || y >= clipw) return;
    // check for overlap
    if ((clipx < z && clipz > x && clipy > w && clipw < y)) return;

    int drawx = std::max(x, clipx);
    int drawy = std::max(y, clipy);
    int draww = std::min(w, clipw);
    int drawz = std::min(z, clipz);
    if (drawz <= drawx || draww <= drawy) return;

    float depth = 0;

    GLfloat Vertices[] = { (float)drawx, (float)drawy, depth,
        (float)drawz, (float)drawy, depth,
        (float)drawz, (float)draww, depth,
        (float)drawx, (float)draww, depth };

    float texx = (drawx - x) / float(width);
    float texy = (drawy - y) / float(height);
    float texz = float(drawz) / z;
    float texw = float(draww) / w;

    GLfloat TexCoord[] = { texx, texy,
        texz, texy,
        texz, texw,
        texx, texw,
    };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
       0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, TexCoord);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_BLEND);

    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        log->Warning("OpenGL: draw texture error: (%u)", err);
    }
}

void RenderOpenGL::DrawTextNew(int x, int y, int width, int h, float u1, float v1, float u2, float v2, Texture *tex, uint32_t colour) {
    // TODO(pskelton): need to add batching here so each lump of text is drawn in one call
    // TODO(pskelton): inputs are color 16 - change param to avoid confusion

    glEnable(GL_TEXTURE_2D);
    colour = Color32(colour);

    float b = (colour & 0xFF) / 255.0f;
    float g = ((colour >> 8) & 0xFF) / 255.0f;
    float r = ((colour >> 16) & 0xFF) / 255.0f;

    glColor3f(r, g, b);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    auto texture = (TextureOpenGL *)tex;
    glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

    int clipx = this->clip_x;
    int clipy = this->clip_y;
    int clipw = this->clip_w;
    int clipz = this->clip_z;

    //int texwidth = texture->GetWidth();
    //int texheight = texture->GetHeight();

    //int width = pSrcRect->z - pSrcRect->x;
    //int height = pSrcRect->w - pSrcRect->y;

    //int x = pTargetPoint->x;  // u* window->GetWidth();
    //int y = pTargetPoint->y;  // v* window->GetHeight();
    int z = x + width;
    int w = y + h;

    // check bounds
    if (x >= (int)window->GetWidth() || x >= clipz || y >= (int)window->GetHeight() || y >= clipw) return;
    // check for overlap
    if (!(clipx < z && clipz > x && clipy < w && clipw > y)) return;

    int drawx = x;  // std::max(x, clipx);
    int drawy = y;  // std::max(y, clipy);
    int draww = w;  // std::min(w, clipw);
    int drawz = z;  // std::min(z, clipz);

    float depth = 0;

    GLfloat Vertices[] = { (float)drawx, (float)drawy, depth,
        (float)drawz, (float)drawy, depth,
        (float)drawz, (float)draww, depth,
        (float)drawx, (float)draww, depth };

    float texx = u1;  // (drawx - x) / float(width);
    float texy = v1;  //  (drawy - y) / float(height);
    float texz = u2;  //  (width - (z - drawz)) / float(width);
    float texw = v2;  // (height - (w - draww)) / float(height);

    GLfloat TexCoord[] = { texx, texy,
        texz, texy,
        texz, texw,
        texx, texw,
    };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
        0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);

    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glTexCoordPointer(2, GL_FLOAT, 0, TexCoord);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;

    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_BLEND);
    //glEnable(GL_DEPTH_TEST);
}

void RenderOpenGL::DrawText(int uOutX, int uOutY, uint8_t* pFontPixels,
                            unsigned int uCharWidth, unsigned int uCharHeight,
                            uint8_t* pFontPalette, uint16_t uFaceColor,
                            uint16_t uShadowColor) {
    return;


    // needs limits checks adding

    //for (uint y = 0; y < uCharHeight; ++y) {
    //    for (uint x = 0; x < uCharWidth; ++x) {
    //        if (*pFontPixels) {
    //            uint16_t color = uShadowColor;
    //            if (*pFontPixels != 1) {
    //                color = uFaceColor;
    //            }
    //            // fontpix[x + y * uCharWidth] = Color32(color);
    //            this->render_target_rgb[(uOutX+x)+(uOutY+y)*window->GetWidth()] = Color32(color);
    //        }
    //        ++pFontPixels;
    //    }
    //}
}

void RenderOpenGL::DrawTextAlpha(int x, int y, unsigned char *font_pixels,
                                 int uCharWidth, unsigned int uFontHeight,
                                 uint8_t *pPalette,
                                 bool present_time_transparency) {
    return;

    // needs limits checks adding

    //if (present_time_transparency) {
    //    for (unsigned int dy = 0; dy < uFontHeight; ++dy) {
    //        for (unsigned int dx = 0; dx < uCharWidth; ++dx) {
    //            uint16_t color = (*font_pixels)
    //                ? pPalette[*font_pixels]
    //                : teal_mask_16;  // transparent color 16bit
    //                          // render->uTargetGMask |
    //                          // render->uTargetBMask;
    //            this->render_target_rgb[(x + dx) + (y + dy) * window->GetWidth()] = Color32(color);
    //            // fontpix[dx + dy * uCharWidth] = Color32(color);
    //            ++font_pixels;
    //        }
    //    }
    //} else {
    //    for (unsigned int dy = 0; dy < uFontHeight; ++dy) {
    //        for (unsigned int dx = 0; dx < uCharWidth; ++dx) {
    //            if (*font_pixels) {
    //                uint8_t index = *font_pixels;
    //                if (index != 255 && index != 1) __debugbreak();
    //                uint8_t r = pPalette[index * 3 + 0];
    //                uint8_t g = pPalette[index * 3 + 1];
    //                uint8_t b = pPalette[index * 3 + 2];
    //                this->render_target_rgb[(x + dx) + (y + dy) * window->GetWidth()] = Color32(r, g, b);
    //                // fontpix[dx + dy * uCharWidth] = Color32(r, g, b);
    //            }
    //            ++font_pixels;
    //        }
    //    }
    //}
}

void RenderOpenGL::Present() {
    window->OpenGlSwapBuffers();
}

// RenderVertexSoft ogl_draw_buildings_vertices[20];


void RenderOpenGL::DrawBuildingsD3D() {
    _set_3d_projection_matrix();
    _set_3d_modelview_matrix();

    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    // int v27;  // eax@57
    int farclip;  // [sp+2Ch] [bp-2Ch]@10
    int nearclip;  // [sp+30h] [bp-28h]@34
    // int v51;  // [sp+34h] [bp-24h]@35
    // int v52;  // [sp+38h] [bp-20h]@36
    int v53;  // [sp+3Ch] [bp-1Ch]@8

    for (BSPModel& model : pOutdoor->pBModels) {
        bool reachable_unused;
        if (!IsBModelVisible(&model, 256, &reachable_unused)) {
            continue;
        }
        model.field_40 |= 1;
        if (model.pFaces.empty()) {
            continue;
        }

        for (ODMFace& face : model.pFaces) {
            if (face.Invisible()) {
                continue;
            }

            v53 = 0;
            auto poly = &array_77EC08[pODMRenderParams->uNumPolygons];

            poly->flags = 0;
            poly->field_32 = 0;
            poly->texture = face.GetTexture();

            if (face.uAttributes & FACE_IsFluid) poly->flags |= 2;
            if (face.uAttributes & FACE_INDOOR_SKY) poly->flags |= 0x400;

            if (face.uAttributes & FACE_FlowDown)
                poly->flags |= 0x400;
            else if (face.uAttributes & FACE_FlowUp)
                poly->flags |= 0x800;

            if (face.uAttributes & FACE_FlowRight)
                poly->flags |= 0x2000;
            else if (face.uAttributes & FACE_FlowLeft)
                poly->flags |= 0x1000;

            poly->sTextureDeltaU = face.sTextureDeltaU;
            poly->sTextureDeltaV = face.sTextureDeltaV;

            unsigned int flow_anim_timer = OS_GetTime() >> 4;
            unsigned int flow_u_mod = poly->texture->GetWidth() - 1;
            unsigned int flow_v_mod = poly->texture->GetHeight() - 1;

            if (face.pFacePlane.vNormal.z && abs(face.pFacePlane.vNormal.z) >= 0.9) {
                if (poly->flags & 0x400)
                    poly->sTextureDeltaV += flow_anim_timer & flow_v_mod;
                if (poly->flags & 0x800)
                    poly->sTextureDeltaV -= flow_anim_timer & flow_v_mod;
            } else {
                if (poly->flags & 0x400)
                    poly->sTextureDeltaV -= flow_anim_timer & flow_v_mod;
                if (poly->flags & 0x800)
                    poly->sTextureDeltaV += flow_anim_timer & flow_v_mod;
            }

            if (poly->flags & 0x1000)
                poly->sTextureDeltaU -= flow_anim_timer & flow_u_mod;
            else if (poly->flags & 0x2000)
                poly->sTextureDeltaU += flow_anim_timer & flow_u_mod;

            nearclip = 0;
            farclip = 0;

            for (uint vertex_id = 1; vertex_id <= face.uNumVertices; vertex_id++) {
                array_73D150[vertex_id - 1].vWorldPosition.x = model.pVertices.pVertices[face.pVertexIDs[vertex_id - 1]].x;
                array_73D150[vertex_id - 1].vWorldPosition.y = model.pVertices.pVertices[face.pVertexIDs[vertex_id - 1]].y;
                array_73D150[vertex_id - 1].vWorldPosition.z = model.pVertices.pVertices[face.pVertexIDs[vertex_id - 1]].z;
                array_73D150[vertex_id - 1].u = (poly->sTextureDeltaU +
                        (__int16)face.pTextureUIDs[vertex_id - 1]) *
                    (1.0 / (double)poly->texture->GetWidth());
                array_73D150[vertex_id - 1].v = (poly->sTextureDeltaV +
                        (__int16)face.pTextureVIDs[vertex_id - 1]) *
                    (1.0 / (double)poly->texture->GetHeight());
            }

            for (uint i = 1; i <= face.uNumVertices; i++) {
                if (model.pVertices.pVertices[face.pVertexIDs[0]].z ==
                    array_73D150[i - 1].vWorldPosition.z)
                    ++v53;
                pCamera3D->ViewTransform(&array_73D150[i - 1], 1);
                //if (array_73D150[i - 1].vWorldViewPosition.x <
                //    pCamera3D->GetNearClip() ||
                //    array_73D150[i - 1].vWorldViewPosition.x >
                //    pCamera3D->GetFarClip()) {
                //    if (array_73D150[i - 1].vWorldViewPosition.x >=
                //        pCamera3D->GetNearClip())
                //        farclip = 1;
                //    else
                //        nearclip = 1;
                //} else {
                    pCamera3D->Project(&array_73D150[i - 1], 1, 0);
                //}
            }

            if (v53 == face.uNumVertices) poly->field_32 |= 1;
            poly->pODMFace = &face;
            poly->uNumVertices = face.uNumVertices;
            poly->field_59 = 5;

            float f = face.pFacePlane.vNormal.x * pOutdoor->vSunlight.x + face.pFacePlane.vNormal.y * pOutdoor->vSunlight.y + face.pFacePlane.vNormal.z * pOutdoor->vSunlight.z;
            poly->dimming_level = 20 - std::round(20 * f);

            if (poly->dimming_level < 0) poly->dimming_level = 0;
            if (poly->dimming_level > 31) poly->dimming_level = 31;
            if (pODMRenderParams->uNumPolygons >= 1999 + 5000) return;
            if (pCamera3D->is_face_faced_to_cameraODM(&face, &array_73D150[0])) {
                face.bVisible = 1;
                poly->uBModelFaceID = face.index;
                poly->uBModelID = model.index;
                poly->pid =
                    PID(OBJECT_BModel, (face.index | (model.index << 6)));
                for (int vertex_id = 0; vertex_id < face.uNumVertices;
                    ++vertex_id) {
                    memcpy(&VertexRenderList[vertex_id],
                        &array_73D150[vertex_id],
                        sizeof(VertexRenderList[vertex_id]));
                    VertexRenderList[vertex_id]._rhw =
                        1.0 / (array_73D150[vertex_id].vWorldViewPosition.x +
                            0.0000001);
                }
                static stru154 static_RenderBuildingsD3D_stru_73C834;

                lightmap_builder->ApplyLights_OutdoorFace(&face);
                decal_builder->ApplyBloodSplat_OutdoorFace(&face);
                lightmap_builder->StationaryLightsCount = 0;
                int v31 = 0;
                if (Lights.uNumLightsApplied > 0 || decal_builder->uNumSplatsThisFace > 0) {
                    v31 = nearclip ? 3 : farclip != 0 ? 5 : 0;

                    // if (face.uAttributes & FACE_OUTLINED) __debugbreak();

                    static_RenderBuildingsD3D_stru_73C834.GetFacePlaneAndClassify(&face, &model.pVertices);
                    if (decal_builder->uNumSplatsThisFace > 0) {
                        decal_builder->BuildAndApplyDecals(
                            31 - poly->dimming_level, 2,
                            &static_RenderBuildingsD3D_stru_73C834,
                            face.uNumVertices, VertexRenderList, (char)v31,
                            -1);
                    }
                }
                if (Lights.uNumLightsApplied > 0)
                    // if (face.uAttributes & FACE_OUTLINED)
                    lightmap_builder->ApplyLights(
                        &Lights, &static_RenderBuildingsD3D_stru_73C834,
                        poly->uNumVertices, VertexRenderList, 0, (char)v31);

                // if (nearclip) {
                //    poly->uNumVertices = ODM_NearClip(face.uNumVertices);
                //    ODM_Project(poly->uNumVertices);
                // }
                // if (farclip) {
                //    poly->uNumVertices = ODM_FarClip(face.uNumVertices);
                //    ODM_Project(poly->uNumVertices);
                // }

                if (poly->uNumVertices) {
                    if (poly->IsWater()) {
                        if (poly->IsWaterAnimDisabled())
                            poly->texture = render->hd_water_tile_anim[0];
                        else
                            poly->texture =
                            render->hd_water_tile_anim
                            [render->hd_water_current_frame];
                    }

                    render->DrawPolygon(poly);
                }
            }
        }
    }
}

void RenderOpenGL::DrawPolygon(struct Polygon *pPolygon) {
    if (pPolygon->uNumVertices < 3) {
        return;
    }

    unsigned int sCorrectedColor;  // [sp+64h] [bp-4h]@4

    auto texture = (TextureOpenGL*)pPolygon->texture;
    ODMFace* pFace = pPolygon->pODMFace;
    auto uNumVertices = pPolygon->uNumVertices;

    if (lightmap_builder->StationaryLightsCount) {
        sCorrectedColor = -1;
    }
    engine->AlterGamma_ODM(pFace, &sCorrectedColor);
    if (_4D864C_force_sw_render_rules && engine->config->Flag1_1()) {
        int v8 = ::GetActorTintColor(
            pPolygon->dimming_level, 0,
            VertexRenderList[0].vWorldViewPosition.x, 0, 0);
        lightmap_builder->DrawLightmaps(v8 /*, 0*/);
    } else {
        if (!lightmap_builder->StationaryLightsCount ||
            _4D864C_force_sw_render_rules && engine->config->Flag1_2()) {
            glDisable(GL_BLEND);
            glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

            glBegin(GL_TRIANGLE_FAN);

            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].pos.x =
                    VertexRenderList[i].vWorldViewProjX;
                d3d_vertex_buffer[i].pos.y =
                    VertexRenderList[i].vWorldViewProjY;
                d3d_vertex_buffer[i].pos.z =
                    1.0 -
                    1.0 / ((VertexRenderList[i].vWorldViewPosition.x * 1000) /
                        pCamera3D->GetFarClip());
                d3d_vertex_buffer[i].rhw =
                    1.0 /
                    (VertexRenderList[i].vWorldViewPosition.x + 0.0000001);
                d3d_vertex_buffer[i].diffuse = ::GetActorTintColor(
                    pPolygon->dimming_level, 0,
                    VertexRenderList[i].vWorldViewPosition.x, 0, 0);
                engine->AlterGamma_ODM(pFace, &d3d_vertex_buffer[i].diffuse);

                if (config->is_using_specular)
                    d3d_vertex_buffer[i].specular = sub_47C3D7_get_fog_specular(
                        0, 0, VertexRenderList[i].vWorldViewPosition.x);
                else
                    d3d_vertex_buffer[i].specular = 0;
                d3d_vertex_buffer[i].texcoord.x = VertexRenderList[i].u;
                d3d_vertex_buffer[i].texcoord.y = VertexRenderList[i].v;
                //}

                if (pFace->uAttributes & FACE_OUTLINED) {
                    int color;
                    if (OS_GetTime() % 300 >= 150)
                        color = 0xFFFF2020;
                    else
                        color = 0xFF901010;

                    // for (uint i = 0; i < uNumVertices; ++i)
                    d3d_vertex_buffer[i].diffuse = color;
                }



                glTexCoord2f(VertexRenderList[i].u,
                    VertexRenderList[i].v);

                glColor4f(
                    ((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                    config->is_using_specular
                    ? ((d3d_vertex_buffer[i].diffuse >> 24) & 0xFF) / 255.0f
                    : 1.0f);

                glVertex3f(VertexRenderList[i].vWorldPosition.x,
                    VertexRenderList[i].vWorldPosition.y,
                    VertexRenderList[i].vWorldPosition.z);
            }

            drawcalls++;
            glEnd();

        } else {
            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].pos.x =
                    VertexRenderList[i].vWorldViewProjX;
                d3d_vertex_buffer[i].pos.y =
                    VertexRenderList[i].vWorldViewProjY;
                d3d_vertex_buffer[i].pos.z =
                    1.0 -
                    1.0 / ((VertexRenderList[i].vWorldViewPosition.x * 1000) /
                        pCamera3D->GetFarClip());
                d3d_vertex_buffer[i].rhw =
                    1.0 /
                    (VertexRenderList[i].vWorldViewPosition.x + 0.0000001);
                d3d_vertex_buffer[i].diffuse = GetActorTintColor(
                    pPolygon->dimming_level, 0,
                    VertexRenderList[i].vWorldViewPosition.x, 0, 0);
                if (config->is_using_specular)
                    d3d_vertex_buffer[i].specular = sub_47C3D7_get_fog_specular(
                        0, 0, VertexRenderList[i].vWorldViewPosition.x);
                else
                    d3d_vertex_buffer[i].specular = 0;
                d3d_vertex_buffer[i].texcoord.x = VertexRenderList[i].u;
                d3d_vertex_buffer[i].texcoord.y = VertexRenderList[i].v;
            }

            glDepthMask(false);
            glBindTexture(GL_TEXTURE_2D, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

            glBegin(GL_TRIANGLE_FAN);  // GL_TRIANGLE_FAN

            for (uint i = 0; i < uNumVertices; ++i) {
                glTexCoord2f(VertexRenderList[i].u, VertexRenderList[i].v);

                glColor4f(
                    ((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                    config->is_using_specular
                    ? ((d3d_vertex_buffer[i].diffuse >> 24) & 0xFF) / 255.0f
                    : 1.0f);

                glVertex3f(VertexRenderList[i].vWorldPosition.x,
                    VertexRenderList[i].vWorldPosition.y,
                    VertexRenderList[i].vWorldPosition.z);
            }

            glEnd();


            drawcalls++;

            glDisable(GL_CULL_FACE);

            // (*(void (**)(void))(*(int *)v50 + 88))();
            lightmap_builder->DrawLightmaps(-1);
            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].diffuse = sCorrectedColor;
            }

            glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

            glDepthMask(true);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

                glBegin(GL_TRIANGLE_FAN);

                for (uint i = 0; i < uNumVertices; ++i) {
                    glTexCoord2f(VertexRenderList[i].u, VertexRenderList[i].v);

                    glColor4f(
                        ((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                        ((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                        ((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                        config->is_using_specular
                        ? ((d3d_vertex_buffer[i].diffuse >> 24) & 0xFF) / 255.0f
                        : 1.0f);

                    glVertex3f(VertexRenderList[i].vWorldPosition.x,
                        VertexRenderList[i].vWorldPosition.y,
                        VertexRenderList[i].vWorldPosition.z);
                }

                glEnd();

            drawcalls++;

            glBlendFunc(GL_ONE, GL_ZERO);
            glDisable(GL_BLEND);
        }
    }

    GL_Check_Errors();
}

void RenderOpenGL::DrawIndoorBatched() { return; }

void RenderOpenGL::DrawIndoorPolygon(unsigned int uNumVertices, BLVFace *pFace,
    int uPackedID, unsigned int uColor,
    int a8) {


    if (uNumVertices < 3) {
        return;
    }

    _set_ortho_projection(1);
    _set_ortho_modelview();

    _set_3d_projection_matrix();
    _set_3d_modelview_matrix();

    unsigned int sCorrectedColor = uColor;

    TextureOpenGL *texture = (TextureOpenGL *)pFace->GetTexture();

    if (lightmap_builder->StationaryLightsCount) {
        sCorrectedColor = 0xFFFFFFFF/*-1*/;
    }

    // perception
    // engine->AlterGamma_BLV(pFace, &sCorrectedColor);

    if (engine->CanSaturateFaces() && (pFace->uAttributes & FACE_IsSecret)) {
        uint eightSeconds = OS_GetTime() % 3000;
        float angle = (eightSeconds / 3000.0f) * 2 * 3.1415f;

        int redstart = (sCorrectedColor & 0x00FF0000) >> 16;

        int col = redstart * abs(cosf(angle));
        // (a << 24) | (r << 16) | (g << 8) | b;
        sCorrectedColor = (0xFF << 24) | (redstart << 16) | (col << 8) | col;
    }

    if (pFace->uAttributes & FACE_OUTLINED) {
        if (OS_GetTime() % 300 >= 150)
            uColor = sCorrectedColor = 0xFF20FF20;
        else
            uColor = sCorrectedColor = 0xFF109010;
        // TODO(pskelton): add debug pick lines in
    }


    if (_4D864C_force_sw_render_rules && engine->config->Flag1_1()) {
        /*
        __debugbreak();
        ErrD3D(pRenderD3D->pDevice->SetRenderState(D3DRENDERSTATE_ZWRITEENABLE,
        false)); ErrD3D(pRenderD3D->pDevice->SetTextureStageState(0,
        D3DTSS_ADDRESS, D3DTADDRESS_WRAP)); for (uint i = 0; i <
        uNumVertices; ++i)
        {
        d3d_vertex_buffer[i].pos.x = array_507D30[i].vWorldViewProjX;
        d3d_vertex_buffer[i].pos.y = array_507D30[i].vWorldViewProjY;
        d3d_vertex_buffer[i].pos.z = 1.0 - 1.0 /
        (array_507D30[i].vWorldViewPosition.x * 0.061758894);
        d3d_vertex_buffer[i].rhw = 1.0 /
        array_507D30[i].vWorldViewPosition.x; d3d_vertex_buffer[i].diffuse =
        sCorrectedColor; d3d_vertex_buffer[i].specular = 0;
        d3d_vertex_buffer[i].texcoord.x = array_507D30[i].u /
        (double)pFace->GetTexture()->GetWidth();
        d3d_vertex_buffer[i].texcoord.y = array_507D30[i].v /
        (double)pFace->GetTexture()->GetHeight();
        }

        ErrD3D(pRenderD3D->pDevice->SetTextureStageState(0, D3DTSS_ADDRESS,
        D3DTADDRESS_WRAP)); ErrD3D(pRenderD3D->pDevice->SetTexture(0,
        nullptr));
        ErrD3D(pRenderD3D->pDevice->DrawPrimitive(D3DPT_TRIANGLEFAN,
        D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1,
        d3d_vertex_buffer, uNumVertices, 28));
        lightmap_builder->DrawLightmaps(-1);
        */
    } else {
        if (!lightmap_builder->StationaryLightsCount || _4D864C_force_sw_render_rules && engine->config->Flag1_2()) {
            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].pos.x = array_507D30[i].vWorldViewProjX;
                d3d_vertex_buffer[i].pos.y = array_507D30[i].vWorldViewProjY;
                d3d_vertex_buffer[i].pos.z =
                    1.0 -
                    1.0 / (array_507D30[i].vWorldViewPosition.x * 0.061758894);
                d3d_vertex_buffer[i].rhw =
                    1.0 / array_507D30[i].vWorldViewPosition.x;
                d3d_vertex_buffer[i].diffuse = sCorrectedColor;
                d3d_vertex_buffer[i].specular = 0;
                d3d_vertex_buffer[i].texcoord.x =
                    array_507D30[i].u / (double)pFace->GetTexture()->GetWidth();
                d3d_vertex_buffer[i].texcoord.y =
                    array_507D30[i].v /
                    (double)pFace->GetTexture()->GetHeight();
            }

            // glEnable(GL_TEXTURE_2D);
            // glDisable(GL_BLEND);
            glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());

            // glDisable(GL_CULL_FACE);  // testing
            // glDisable(GL_DEPTH_TEST);

            // if (uNumVertices != 3 ) return; //3 ,4, 5 ,6

            glBegin(GL_TRIANGLE_FAN);

            for (uint i = 0; i < pFace->uNumVertices; ++i) {
                glTexCoord2f(d3d_vertex_buffer[i].texcoord.x, d3d_vertex_buffer[i].texcoord.y);
                //glTexCoord2f(((pFace->pVertexUIDs[i] + Lights.pDeltaUV[0]) / (double)pFace->GetTexture()->GetWidth()), ((pFace->pVertexVIDs[i] + Lights.pDeltaUV[1]) / (double)pFace->GetTexture()->GetHeight()));

                glColor4f(
                     (float)((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                     (float)((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                     (float)((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                    1.0f);

                glVertex3f(pIndoor->pVertices[pFace->pVertexIDs[i]].x,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].y,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].z);
            }
            drawcalls++;

            glEnd();
        } else {
            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].pos.x = array_507D30[i].vWorldViewProjX;
                d3d_vertex_buffer[i].pos.y = array_507D30[i].vWorldViewProjY;
                d3d_vertex_buffer[i].pos.z =
                    1.0 -
                    1.0 / (array_507D30[i].vWorldViewPosition.x * 0.061758894);
                d3d_vertex_buffer[i].rhw = 1.0 / array_507D30[i].vWorldViewPosition.x;
                d3d_vertex_buffer[i].diffuse = uColor;
                d3d_vertex_buffer[i].specular = 0;
                d3d_vertex_buffer[i].texcoord.x =
                    array_507D30[i].u / (double)pFace->GetTexture()->GetWidth();
                d3d_vertex_buffer[i].texcoord.y =
                    array_507D30[i].v /
                    (double)pFace->GetTexture()->GetHeight();
            }

            glDepthMask(false);

            // ErrD3D(pRenderD3D->pDevice->SetTextureStageState(0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP));
            glBindTexture(GL_TEXTURE_2D, 0);

            glBegin(GL_TRIANGLE_FAN);

            for (uint i = 0; i < pFace->uNumVertices; ++i) {
                glTexCoord2f(d3d_vertex_buffer[i].texcoord.x, d3d_vertex_buffer[i].texcoord.y);
                //glTexCoord2f(((pFace->pVertexUIDs[i] + Lights.pDeltaUV[0]) / (double)pFace->GetTexture()->GetWidth()), ((pFace->pVertexVIDs[i] + Lights.pDeltaUV[1]) / (double)pFace->GetTexture()->GetHeight()));


                glColor4f(
                    ((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                    1.0f);

                glVertex3f(pIndoor->pVertices[pFace->pVertexIDs[i]].x,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].y,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].z);
            }
            drawcalls++;

            glEnd();
            glDisable(GL_CULL_FACE);

            lightmap_builder->DrawLightmaps(-1 /*, 0*/);

            for (uint i = 0; i < uNumVertices; ++i) {
                d3d_vertex_buffer[i].diffuse = sCorrectedColor;
            }

            glBindTexture(GL_TEXTURE_2D, texture->GetOpenGlTexture());
            // ErrD3D(pRenderD3D->pDevice->SetTextureStageState(0, D3DTSS_ADDRESS, D3DTADDRESS_WRAP));

            glDepthMask(true);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);

            glBegin(GL_TRIANGLE_FAN);

            for (uint i = 0; i < pFace->uNumVertices; ++i) {
                glTexCoord2f(d3d_vertex_buffer[i].texcoord.x, d3d_vertex_buffer[i].texcoord.y);
                //glTexCoord2f(((pFace->pVertexUIDs[i] + Lights.pDeltaUV[0]) / (double)pFace->GetTexture()->GetWidth()), ((pFace->pVertexVIDs[i] + Lights.pDeltaUV[1]) / (double)pFace->GetTexture()->GetHeight()));


                glColor4f(
                    ((d3d_vertex_buffer[i].diffuse >> 16) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 8) & 0xFF) / 255.0f,
                    ((d3d_vertex_buffer[i].diffuse >> 0) & 0xFF) / 255.0f,
                    1.0f);

                glVertex3f(pIndoor->pVertices[pFace->pVertexIDs[i]].x,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].y,
                    pIndoor->pVertices[pFace->pVertexIDs[i]].z);
            }
            drawcalls++;

            glEnd();

            glDisable(GL_BLEND);
        }
    }

    GL_Check_Errors();
}

bool RenderOpenGL::SwitchToWindow() {
    // pParty->uFlags |= PARTY_FLAGS_1_ForceRedraw;
    pViewport->ResetScreen();
    CreateZBuffer();

    return true;
}


bool RenderOpenGL::Initialize() {
    if (!RenderBase::Initialize()) {
        return false;
    }

    if (window != nullptr) {
        window->OpenGlCreate();

        glShadeModel(GL_SMOOTH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);       // Black Background
        glClearDepth(1.0f);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glViewport(0, 0, window->GetWidth(), window->GetHeight());
        glScissor(0, 0, window->GetWidth(), window->GetHeight());

        glEnable(GL_SCISSOR_TEST);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();

        // Calculate The Aspect Ratio Of The Window
        gluPerspective(45.0f,
            (GLfloat)window->GetWidth() / (GLfloat)window->GetHeight(),
            0.1f, 100.0f);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        // Swap Buffers (Double Buffering)
        window->OpenGlSwapBuffers();

        this->clip_x = this->clip_y = 0;
        this->clip_z = window->GetWidth();
        this->clip_w = window->GetHeight();

        PostInitialization();

        GL_Check_Errors();

        // check gpu gl capability params
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &GPU_MAX_TEX_SIZE);
        assert(GPU_MAX_TEX_SIZE >= 512);

        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &GPU_MAX_TEX_LAYERS);
        assert(GPU_MAX_TEX_LAYERS >= 256);

        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &GPU_MAX_TEX_UNITS);
        assert(GPU_MAX_TEX_UNITS >= 16);

        glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, &GPU_MAX_UNIFORM_COMP);
        assert(GPU_MAX_UNIFORM_COMP >= 1024);

        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &GPU_MAX_TOTAL_TEXTURES);
        assert(GPU_MAX_TOTAL_TEXTURES >= 80);

        GL_Check_Errors();

        // initiate shaders
        if (!InitShaders()) {
            logger->Warning("--- Shader initialisation has failed ---");
            return false;
        }

        return true;
    }

    return false;
}

void RenderOpenGL::WritePixel16(int x, int y, uint16_t color) {
    return;
}

void RenderOpenGL::FillRectFast(unsigned int uX, unsigned int uY,
                                unsigned int uWidth, unsigned int uHeight,
                                unsigned int uColor16) {
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);

    unsigned int b = (uColor16 & 0x1F) * 8;
    unsigned int g = ((uColor16 >> 5) & 0x3F) * 4;
    unsigned int r = ((uColor16 >> 11) & 0x1F) * 8;
    glColor3ub(r, g, b);

    float depth = 0;

    GLfloat Vertices[] = { (float)uX, (float)uY, depth,
        (float)(uX+uWidth), (float)uY, depth,
        (float)(uX + uWidth), (float)(uY+uHeight), depth,
        (float)uX, (float)(uY + uHeight), depth };

    GLubyte indices[] = { 0, 1, 2,  // first triangle (bottom left - top left - top right)
        0, 2, 3 };  // second triangle (bottom left - top right - bottom right)

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, Vertices);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, indices);
    drawcalls++;
    glDisableClientState(GL_VERTEX_ARRAY);

    GL_Check_Errors();
}

// gl shaders
bool RenderOpenGL::InitShaders() {
    logger->Info("Building outdoors terrain shader...");
    terrainshader.build("../../../../Engine/Graphics/Shaders/glterrain.vs", "../../../../Engine/Graphics/Shaders/glterrain.fs");
    if (terrainshader.ID == 0)
        return false;

    return true;
}

void RenderOpenGL::ReleaseTerrain() {
    /*GLuint terrainVBO, terrainVAO;
    GLuint terraintextures[8];
    uint numterraintexloaded[8];
    uint terraintexturesizes[8];
    std::map<std::string, int> terraintexmap;*/

    terraintexmap.clear();

    for (int i = 0; i < 8; i++) {
        glDeleteTextures(1, &terraintextures[i]);
        terraintextures[i] = 0;
        numterraintexloaded[i] = 0;
        terraintexturesizes[i] = 0;
    }

    glDeleteBuffers(1, &terrainVBO);
    glDeleteVertexArrays(1, &terrainVAO);

    terrainVBO = 0;
    terrainVAO = 0;

    GL_Check_Errors();
}


bool RenderOpenGL::NuklearInitialize(struct nk_tex_font *tfont) {
    struct nk_context* nk_ctx = nuklear->ctx;
    if (!nk_ctx) {
        log->Warning("Nuklear context is not available");
        return false;
    }

    if (!NuklearCreateDevice()) {
        log->Warning("Nuklear device creation failed");
        NuklearRelease();
        return false;
    }

    nk_font_atlas_init_default(&nk_dev.atlas);
    struct nk_tex_font *font = NuklearFontLoad(NULL, 13);
    nk_dev.atlas.default_font = font->font;
    if (!nk_dev.atlas.default_font) {
        log->Warning("Nuklear default font loading failed");
        NuklearRelease();
        return false;
    }

    memcpy(tfont, font, sizeof(struct nk_tex_font));

    if (!nk_init_default(nk_ctx, &nk_dev.atlas.default_font->handle)) {
        log->Warning("Nuklear initialization failed");
        NuklearRelease();
        return false;
    }

    nk_buffer_init_default(&nk_dev.cmds);

    return true;
}

bool RenderOpenGL::NuklearCreateDevice() {
    GLint status;
    static const GLchar* vertex_shader =
        NK_SHADER_VERSION
        "uniform mat4 ProjMtx;\n"
        "in vec2 Position;\n"
        "in vec2 TexCoord;\n"
        "in vec4 Color;\n"
        "out vec2 Frag_UV;\n"
        "out vec4 Frag_Color;\n"
        "void main() {\n"
        "   Frag_UV = TexCoord;\n"
        "   Frag_Color = Color;\n"
        "   gl_Position = ProjMtx * vec4(Position.xy, 0, 1);\n"
        "}\n";
    static const GLchar* fragment_shader =
        NK_SHADER_VERSION
        "precision mediump float;\n"
        "uniform sampler2D Texture;\n"
        "in vec2 Frag_UV;\n"
        "in vec4 Frag_Color;\n"
        "out vec4 Out_Color;\n"
        "void main(){\n"
        "   Out_Color = Frag_Color * texture(Texture, Frag_UV.st);\n"
        "}\n";

    nk_buffer_init_default(&nk_dev.cmds);
    nk_dev.prog = glCreateProgram();
    nk_dev.vert_shdr = glCreateShader(GL_VERTEX_SHADER);
    nk_dev.frag_shdr = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(nk_dev.vert_shdr, 1, &vertex_shader, 0);
    glShaderSource(nk_dev.frag_shdr, 1, &fragment_shader, 0);
    glCompileShader(nk_dev.vert_shdr);
    glCompileShader(nk_dev.frag_shdr);
    glGetShaderiv(nk_dev.vert_shdr, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
        return false;
    glGetShaderiv(nk_dev.frag_shdr, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE)
        return false;
    glAttachShader(nk_dev.prog, nk_dev.vert_shdr);
    glAttachShader(nk_dev.prog, nk_dev.frag_shdr);
    glLinkProgram(nk_dev.prog);
    glGetProgramiv(nk_dev.prog, GL_LINK_STATUS, &status);
    if (status != GL_TRUE)
        return false;

    nk_dev.uniform_tex = glGetUniformLocation(nk_dev.prog, "Texture");
    nk_dev.uniform_proj = glGetUniformLocation(nk_dev.prog, "ProjMtx");
    nk_dev.attrib_pos = glGetAttribLocation(nk_dev.prog, "Position");
    nk_dev.attrib_uv = glGetAttribLocation(nk_dev.prog, "TexCoord");
    nk_dev.attrib_col = glGetAttribLocation(nk_dev.prog, "Color");

    {
        GLsizei vs = sizeof(struct nk_vertex);
        size_t vp = offsetof(struct nk_vertex, position);
        size_t vt = offsetof(struct nk_vertex, uv);
        size_t vc = offsetof(struct nk_vertex, col);

        glGenBuffers(1, &nk_dev.vbo);
        glGenBuffers(1, &nk_dev.ebo);
        glGenVertexArrays(1, &nk_dev.vao);

        glBindVertexArray(nk_dev.vao);
        glBindBuffer(GL_ARRAY_BUFFER, nk_dev.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nk_dev.ebo);

        glEnableVertexAttribArray((GLuint)nk_dev.attrib_pos);
        glEnableVertexAttribArray((GLuint)nk_dev.attrib_uv);
        glEnableVertexAttribArray((GLuint)nk_dev.attrib_col);

        glVertexAttribPointer((GLuint)nk_dev.attrib_pos, 2, GL_FLOAT, GL_FALSE, vs, (void*)vp);
        glVertexAttribPointer((GLuint)nk_dev.attrib_uv, 2, GL_FLOAT, GL_FALSE, vs, (void*)vt);
        glVertexAttribPointer((GLuint)nk_dev.attrib_col, 4, GL_UNSIGNED_BYTE, GL_TRUE, vs, (void*)vc);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    GL_Check_Errors();
    return true;
}

bool RenderOpenGL::NuklearRender(enum nk_anti_aliasing AA, int max_vertex_buffer, int max_element_buffer) {
    struct nk_context *nk_ctx = nuklear->ctx;
    if (!nk_ctx)
        return false;

    int width, height;
    int display_width, display_height;
    struct nk_vec2 scale {};
    GLfloat ortho[4][4] = {
        { 2.0f,  0.0f,  0.0f,  0.0f },
        { 0.0f, -2.0f,  0.0f,  0.0f },
        { 0.0f,  0.0f, -1.0f,  0.0f },
        { -1.0f, 1.0f,  0.0f,  1.0f },
    };

    height = window->GetHeight();
    width = window->GetWidth();
    display_height = render->GetRenderHeight();
    display_width = render->GetRenderWidth();

    ortho[0][0] /= (GLfloat)width;
    ortho[1][1] /= (GLfloat)height;

    scale.x = (float)display_width / (float)width;
    scale.y = (float)display_height / (float)height;

    /* setup global state */
    glViewport(0, 0, display_width, display_height);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glActiveTexture(GL_TEXTURE0);

    /* setup program */
    glUseProgram(nk_dev.prog);
    glUniform1i(nk_dev.uniform_tex, 0);
    glUniformMatrix4fv(nk_dev.uniform_proj, 1, GL_FALSE, &ortho[0][0]);
    {
        /* convert from command queue into draw list and draw to screen */
        const struct nk_draw_command *cmd;
        void *vertices, *elements;
        const nk_draw_index *offset = NULL;
        struct nk_buffer vbuf, ebuf;

        /* allocate vertex and element buffer */
        glBindVertexArray(nk_dev.vao);
        glBindBuffer(GL_ARRAY_BUFFER, nk_dev.vbo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nk_dev.ebo);

        glBufferData(GL_ARRAY_BUFFER, max_vertex_buffer, NULL, GL_STREAM_DRAW);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, max_element_buffer, NULL, GL_STREAM_DRAW);

        /* load vertices/elements directly into vertex/element buffer */
        vertices = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
        elements = glMapBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_WRITE_ONLY);
        {
            /* fill convert configuration */
            struct nk_convert_config config;
            struct nk_draw_vertex_layout_element vertex_layout[] = {
                {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_vertex, position)},
                {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_vertex, uv)},
                {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_vertex, col)},
                {NK_VERTEX_LAYOUT_END}
            };
            memset(&config, 0, sizeof(config));
            config.vertex_layout = vertex_layout;
            config.vertex_size = sizeof(struct nk_vertex);
            config.vertex_alignment = NK_ALIGNOF(struct nk_vertex);
            config.null = nk_dev.null;
            config.circle_segment_count = 22;
            config.curve_segment_count = 22;
            config.arc_segment_count = 22;
            config.global_alpha = 1.0f;
            config.shape_AA = AA;
            config.line_AA = AA;

            /* setup buffers to load vertices and elements */
            nk_buffer_init_fixed(&vbuf, vertices, (nk_size)max_vertex_buffer);
            nk_buffer_init_fixed(&ebuf, elements, (nk_size)max_element_buffer);
            nk_convert(nk_ctx, &nk_dev.cmds, &vbuf, &ebuf, &config);
        }
        glUnmapBuffer(GL_ARRAY_BUFFER);
        glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);

        /* iterate over and execute each draw command */
        nk_draw_foreach(cmd, nk_ctx, &nk_dev.cmds) {
            if (!cmd->elem_count) continue;
            glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
            glScissor((GLint)(cmd->clip_rect.x * scale.x),
                (GLint)((height - (GLint)(cmd->clip_rect.y + cmd->clip_rect.h)) * scale.y),
                (GLint)(cmd->clip_rect.w * scale.x),
                (GLint)(cmd->clip_rect.h * scale.y));
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_SHORT, offset);
            offset += cmd->elem_count;
        }
        nk_clear(nk_ctx);
        nk_buffer_clear(&nk_dev.cmds);
    }

    glUseProgram(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    // glDisable(GL_SCISSOR_TEST);

    GL_Check_Errors();
    return true;
}

void RenderOpenGL::NuklearRelease() {
    nk_font_atlas_clear(&nk_dev.atlas);

    glDetachShader(nk_dev.prog, nk_dev.vert_shdr);
    glDetachShader(nk_dev.prog, nk_dev.frag_shdr);
    glDeleteShader(nk_dev.vert_shdr);
    glDeleteShader(nk_dev.frag_shdr);
    glDeleteProgram(nk_dev.prog);
    glDeleteBuffers(1, &nk_dev.vbo);
    glDeleteBuffers(1, &nk_dev.ebo);
    glDeleteVertexArrays(1, &nk_dev.vao);

    GL_Check_Errors();

    nk_buffer_free(&nk_dev.cmds);

    memset(&nk_dev, 0, sizeof(nk_dev));
}

struct nk_tex_font *RenderOpenGL::NuklearFontLoad(const char* font_path, size_t font_size) {
    const void *image;
    int w, h;
    GLuint texid;

    struct nk_tex_font *tfont = new (struct nk_tex_font);
    if (!tfont)
        return NULL;

    struct nk_font_config cfg = nk_font_config(font_size);
    cfg.merge_mode = nk_false;
    cfg.coord_type = NK_COORD_UV;
    cfg.spacing = nk_vec2(0, 0);
    cfg.oversample_h = 3;
    cfg.oversample_v = 1;
    cfg.range = nk_font_cyrillic_glyph_ranges();
    cfg.size = font_size;
    cfg.pixel_snap = 0;
    cfg.fallback_glyph = '?';

    nk_font_atlas_begin(&nk_dev.atlas);

    if (!font_path)
        tfont->font = nk_font_atlas_add_default(&nk_dev.atlas, font_size, 0);
    else
        tfont->font = nk_font_atlas_add_from_file(&nk_dev.atlas, font_path, font_size, &cfg);

    image = nk_font_atlas_bake(&nk_dev.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);

    glGenTextures(1, &texid);
    glBindTexture(GL_TEXTURE_2D, texid);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, image);

    tfont->texid = texid;
    nk_font_atlas_end(&nk_dev.atlas, nk_handle_id(texid), &nk_dev.null);

    GL_Check_Errors();
    return tfont;
}

void RenderOpenGL::NuklearFontFree(struct nk_tex_font *tfont) {
    if (tfont)
        glDeleteTextures(1, &tfont->texid);
    GL_Check_Errors();
}

struct nk_image RenderOpenGL::NuklearImageLoad(Image *img) {
    GLuint texid;
    auto t = (TextureOpenGL *)img;
    //unsigned __int8 *pixels = (unsigned __int8 *)t->GetPixels(IMAGE_FORMAT_A8R8G8B8);
    texid = t->GetOpenGlTexture();

    //glGenTextures(1, &texid);
    t->SetOpenGlTexture(texid);
    //glBindTexture(GL_TEXTURE_2D, texid);
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    //glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    //glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, t->GetWidth(), t->GetHeight(), 0, /*GL_RGBA*/GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    //glBindTexture(GL_TEXTURE_2D, 0);

    GL_Check_Errors();
    return nk_image_id(texid);
}

void RenderOpenGL::NuklearImageFree(Image *img) {
    auto t = (TextureOpenGL *)img;
    GLuint texid = t->GetOpenGlTexture();
    if (texid != -1) {
        glDeleteTextures(1, &texid);
    }
    GL_Check_Errors();
}
