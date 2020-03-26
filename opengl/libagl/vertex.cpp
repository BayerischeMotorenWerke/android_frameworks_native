/* libs/opengles/vertex.cpp
**
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License"); 
** you may not use this file except in compliance with the License. 
** You may obtain a copy of the License at 
**
**     http://www.apache.org/licenses/LICENSE-2.0 
**
** Unless required by applicable law or agreed to in writing, software 
** distributed under the License is distributed on an "AS IS" BASIS, 
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
** See the License for the specific language governing permissions and 
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include "context.h"
#include "fp.h"
#include "vertex.h"
#include "state.h"
#include "matrix.h"

namespace android {

// ----------------------------------------------------------------------------

void ogles_init_vertex(ogles_context_t* c)
{
    c->cull.enable = GL_FALSE;
    c->cull.cullFace = GL_BACK;
    c->cull.frontFace = GL_CCW;

    c->current.color.r = 0x10000;
    c->current.color.g = 0x10000;
    c->current.color.b = 0x10000;
    c->current.color.a = 0x10000;

    c->currentNormal.z = 0x10000;
}

void ogles_uninit_vertex(ogles_context_t* /*c*/)
{
}

// ----------------------------------------------------------------------------
// vertex processing
// ----------------------------------------------------------------------------

// Divides a vertex clip coordinates by W
static inline
void perspective(ogles_context_t* c, vertex_t* v, uint32_t enables)
{
    // [x,y,z]window = vpt * ([x,y,z]clip / clip.w)
    // [w]window = 1/w

    // With a regular projection generated by glFrustum(),
    // we have w=-z, therefore, w is in [zNear, zFar].
    // Also, zNear and zFar are stricly positive,
    // and 1/w (window.w) is in [1/zFar, 1/zNear], usually this
    // means ]0, +inf[ -- however, it is always recommended
    // to use as large values as possible for zNear.
    // All in all, w is usually smaller than 1.0 (assuming
    // zNear is at least 1.0); and even if zNear is smaller than 1.0
    // values of w won't be too big.

    const int32_t rw = gglRecip28(v->clip.w);
    const GLfixed* const m = c->transforms.vpt.transform.matrix.m;
    v->window.w = rw;
    v->window.x = gglMulAddx(gglMulx(v->clip.x, rw, 16), m[ 0], m[12], 28); 
    v->window.y = gglMulAddx(gglMulx(v->clip.y, rw, 16), m[ 5], m[13], 28);
    v->window.x = TRI_FROM_FIXED(v->window.x);
    v->window.y = TRI_FROM_FIXED(v->window.y);
    if (enables & GGL_ENABLE_DEPTH_TEST) {
        v->window.z = gglMulAddx(gglMulx(v->clip.z, rw, 16), m[10], m[14], 28);
    }
}

// frustum clipping and W-divide
static inline
void clipFrustumPerspective(ogles_context_t* c, vertex_t* v, uint32_t enables)
{
    // ndc = clip / W
    // window = ncd * viewport
    
    // clip to the view-volume
    uint32_t clip = v->flags & vertex_t::CLIP_ALL;
    const GLfixed w = v->clip.w;
    if (v->clip.x < -w)   clip |= vertex_t::CLIP_L;
    if (v->clip.x >  w)   clip |= vertex_t::CLIP_R;
    if (v->clip.y < -w)   clip |= vertex_t::CLIP_B;
    if (v->clip.y >  w)   clip |= vertex_t::CLIP_T;
    if (v->clip.z < -w)   clip |= vertex_t::CLIP_N;
    if (v->clip.z >  w)   clip |= vertex_t::CLIP_F;

    v->flags |= clip;
    c->arrays.cull &= clip;

    if (ggl_likely(!clip)) {
        // if the vertex is clipped, we don't do the perspective
        // divide, since we don't need its window coordinates.
        perspective(c, v, enables);
    }
}

// frustum clipping, user clipping and W-divide
static inline
void clipAllPerspective(ogles_context_t* c, vertex_t* v, uint32_t enables)
{
    // compute eye coordinates
    c->arrays.mv_transform(
            &c->transforms.modelview.transform, &v->eye, &v->obj);
    v->flags |= vertex_t::EYE;

    // clip this vertex against each user clip plane
    uint32_t clip = 0;
    int planes = c->clipPlanes.enable;
    while (planes) {
        const int i = 31 - gglClz(planes);
        planes &= ~(1<<i);
        // XXX: we should have a special dot() for 2,3,4 coords vertices
        GLfixed d = dot4(c->clipPlanes.plane[i].equation.v, v->eye.v);
        if (d < 0) {
            clip |= 0x100<<i;
        }
    }
    v->flags |= clip;

    clipFrustumPerspective(c, v, enables);
}

// ----------------------------------------------------------------------------

void ogles_vertex_project(ogles_context_t* c, vertex_t* v) {
    perspective(c, v, c->rasterizer.state.enables);
}

void ogles_vertex_perspective2D(ogles_context_t* c, vertex_t* v)
{
    // here we assume w=1.0 and the viewport transformation
    // has been applied already.
    c->arrays.cull = 0;
    v->window.x = TRI_FROM_FIXED(v->clip.x);
    v->window.y = TRI_FROM_FIXED(v->clip.y);
    v->window.z = v->clip.z;
    v->window.w = v->clip.w << 12;
}

void ogles_vertex_perspective3DZ(ogles_context_t* c, vertex_t* v) {
    clipFrustumPerspective(c, v, GGL_ENABLE_DEPTH_TEST);
}
void ogles_vertex_perspective3D(ogles_context_t* c, vertex_t* v) {
    clipFrustumPerspective(c, v, 0);
}
void ogles_vertex_clipAllPerspective3DZ(ogles_context_t* c, vertex_t* v) {
    clipAllPerspective(c, v, GGL_ENABLE_DEPTH_TEST);
}
void ogles_vertex_clipAllPerspective3D(ogles_context_t* c, vertex_t* v) {
    clipAllPerspective(c, v, 0);
}

static void clipPlanex(GLenum plane, const GLfixed* equ, ogles_context_t* c)
{
    const int p = plane - GL_CLIP_PLANE0;
    if (ggl_unlikely(uint32_t(p) > (GL_CLIP_PLANE5 - GL_CLIP_PLANE0))) {
        ogles_error(c, GL_INVALID_ENUM);
        return;
    }

    vec4_t& equation = c->clipPlanes.plane[p].equation;
    memcpy(equation.v, equ, sizeof(vec4_t));

    ogles_validate_transform(c, transform_state_t::MVIT);
    transform_t& mvit = c->transforms.mvit4;
    mvit.point4(&mvit, &equation, &equation);
}

// ----------------------------------------------------------------------------
}; // namespace android
// ----------------------------------------------------------------------------

using namespace android;


void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    ogles_context_t* c = ogles_context_t::get();
    c->current.color.r       = gglFloatToFixed(r);
    c->currentColorClamped.r = gglClampx(c->current.color.r);
    c->current.color.g       = gglFloatToFixed(g);
    c->currentColorClamped.g = gglClampx(c->current.color.g);
    c->current.color.b       = gglFloatToFixed(b);
    c->currentColorClamped.b = gglClampx(c->current.color.b);
    c->current.color.a       = gglFloatToFixed(a);
    c->currentColorClamped.a = gglClampx(c->current.color.a);
}

void glColor4x(GLfixed r, GLfixed g, GLfixed b, GLfixed a)
{
    ogles_context_t* c = ogles_context_t::get();
    c->current.color.r = r;
    c->current.color.g = g;
    c->current.color.b = b;
    c->current.color.a = a;
    c->currentColorClamped.r = gglClampx(r);
    c->currentColorClamped.g = gglClampx(g);
    c->currentColorClamped.b = gglClampx(b);
    c->currentColorClamped.a = gglClampx(a);
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    ogles_context_t* c = ogles_context_t::get();
    c->currentNormal.x = gglFloatToFixed(x);
    c->currentNormal.y = gglFloatToFixed(y);
    c->currentNormal.z = gglFloatToFixed(z);
}

void glNormal3x(GLfixed x, GLfixed y, GLfixed z)
{
    ogles_context_t* c = ogles_context_t::get();
    c->currentNormal.x = x;
    c->currentNormal.y = y;
    c->currentNormal.z = z;
}

// ----------------------------------------------------------------------------

void glClipPlanef(GLenum plane, const GLfloat* equ)
{
    const GLfixed equx[4] = {
            gglFloatToFixed(equ[0]),
            gglFloatToFixed(equ[1]),
            gglFloatToFixed(equ[2]),
            gglFloatToFixed(equ[3])
    };
    ogles_context_t* c = ogles_context_t::get();
    clipPlanex(plane, equx, c);
}

void glClipPlanex(GLenum plane, const GLfixed* equ)
{
    ogles_context_t* c = ogles_context_t::get();
    clipPlanex(plane, equ, c);
}