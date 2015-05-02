// milton.h
// (c) Copyright 2015 by Sergio Gonzalez


// Rename types for convenience
typedef int8_t      int8;
typedef uint8_t     uint8;
typedef int16_t     int16;
typedef uint16_t    uint16;
typedef int32_t     int32;
typedef uint32_t    uint32;
typedef int64_t     int64;
typedef uint64_t    uint64;
typedef int32_t     bool32;

#if defined(_MSC_VER)
#define true 1
#define false 0
#endif


#define stack_count(arr) (sizeof((arr)) / sizeof((arr)[0]))


#include <math.h>  // powf
#include <float.h>

#include "libserg/gl_helpers.h"
#include "vector.generated.h"  // Generated by metaprogram

#include "utils.h"
#include "color.h"



typedef struct Brush_s
{
    int32 radius;  // This should be replaced by a BrushType and some union containing brush info.
    v3f   color;
    float alpha;
} Brush;

#define LIMIT_STROKE_POINTS 1024
typedef struct Stroke_s
{
    Brush   brush;
    // TODO turn this into a deque??
    v2i     points[LIMIT_STROKE_POINTS];
    v2i     clipped_points[2 * LIMIT_STROKE_POINTS];  // Clipped points are in the form [ab bc cd df]
                                                        // That's why we double the space for them.
    int32   num_points;
    int32   num_clipped_points;
    Rect    bounds;
} Stroke;

typedef struct MiltonGLState_s
{
    GLuint quad_program;
    GLuint texture;
    GLuint quad_vao;
} MiltonGLState;

typedef struct MiltonState_s
{
    int32_t     full_width;             // Dimensions of the raster
    int32_t     full_height;
    uint8_t     bytes_per_pixel;
    uint8_t*    raster_buffer;
    size_t      raster_buffer_size;

    MiltonGLState* gl;

    ColorPicker picker;

    v2i screen_size;

    // Maps screen_size to a rectangle in our infinite canvas.
    int32 view_scale;

    v2i     last_point;  // Last input point. Used to determine area to update.
    Stroke  working_stroke;

    Stroke  strokes[4096];  // TODO: Create a deque to store arbitrary number of strokes.
    int32   num_strokes;

    // Heap
    Arena*      root_arena;         // Persistent memory.
    Arena*      transient_arena;    // Gets reset after every call to milton_update().

} MiltonState;

typedef struct MiltonInput_s
{
    bool32 full_refresh;
    bool32 reset;
    bool32 end_stroke;
    v2i* point;
    int scale;
} MiltonInput;

static void milton_gl_backend_draw(MiltonState* milton_state)
{
    MiltonGLState* gl = milton_state->gl;
    glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA,
            milton_state->screen_size.w, milton_state->screen_size.h,
            0, GL_RGBA, GL_UNSIGNED_BYTE, (GLvoid*)milton_state->raster_buffer);
    glUseProgram(gl->quad_program);
    glBindVertexArray(gl->quad_vao);
    GLCHK (glDrawArrays (GL_TRIANGLE_FAN, 0, 4) );
}

static void milton_gl_backend_init(MiltonState* milton_state)
{
    // Init quad program
    {
        const char* shader_contents[2];

        shader_contents[0] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "layout(location = 0) in vec2 position;\n"
            "\n"
            "out vec2 coord;\n"
            "\n"
            "void main()\n"
            "{\n"
            "   coord = (position + vec2(1,1))/2;\n"
            "   coord.y = 1 - coord.y;"
            "   // direct to clip space. must be in [-1, 1]^2\n"
            "   gl_Position = vec4(position, 0.0, 1.0);\n"
            "}\n";


        shader_contents[1] =
            "#version 330\n"
            "#extension GL_ARB_explicit_uniform_location : enable\n"
            "\n"
            "layout(location = 1) uniform sampler2D buffer;\n"
            "in vec2 coord;\n"
            "out vec4 out_color;\n"
            "\n"
            "vec3 sRGB_to_linear(vec3 rgb)\n"
            "{\n"
                "vec3 result = pow((rgb + vec3(0.055)) / vec3(1.055), vec3(2.4));\n"
                "return result;\n"
            "}\n"
            "void main(void)\n"
            "{\n"
            "   out_color = texture(buffer, coord);"
            "   out_color = vec4(sRGB_to_linear(out_color.rgb), 1);"
            "}\n";

        GLuint shader_objects[2] = {0};
        for ( int i = 0; i < 2; ++i )
        {
            GLuint shader_type = (i == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
            shader_objects[i] = gl_compile_shader(shader_contents[i], shader_type);
        }
        milton_state->gl->quad_program = glCreateProgram();
        gl_link_program(milton_state->gl->quad_program, shader_objects, 2);

        glUseProgram(milton_state->gl->quad_program);
        glUniform1i(1, 0 /*GL_TEXTURE0*/);
    }

    // Create texture
    {
        GLCHK (glActiveTexture (GL_TEXTURE0) );
        // Create texture
        GLCHK (glGenTextures   (1, &milton_state->gl->texture));
        GLCHK (glBindTexture   (GL_TEXTURE_2D, milton_state->gl->texture));

        // Note for the future: These are needed.
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER));
        GLCHK (glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER));

        // Pass a null pointer, texture will be filled by opencl ray tracer
        GLCHK ( glTexImage2D(
                    GL_TEXTURE_2D, 0, GL_RGBA,
                    milton_state->screen_size.w, milton_state->screen_size.h,
                    0, GL_RGBA, GL_FLOAT, NULL) );
    }
    // Create quad
    {
        //const GLfloat u = 1.0f;
#define u -1.0f
        // full
        GLfloat vert_data[] =
        {
            -u, u,
            -u, -u,
            u, -u,
            u, u,
        };
#undef u
        GLCHK (glGenVertexArrays(1, &milton_state->gl->quad_vao));
        GLCHK (glBindVertexArray(milton_state->gl->quad_vao));

        GLuint vbo;
        GLCHK (glGenBuffers(1, &vbo));
        GLCHK (glBindBuffer(GL_ARRAY_BUFFER, vbo));

        GLCHK (glBufferData (GL_ARRAY_BUFFER, sizeof(vert_data), vert_data, GL_STATIC_DRAW));
        GLCHK (glEnableVertexAttribArray (0) );
        GLCHK (glVertexAttribPointer     (/*attrib location*/0,
                    /*size*/2, GL_FLOAT, /*normalize*/GL_FALSE, /*stride*/0, /*ptr*/0));
    }
}

static void milton_init(MiltonState* milton_state)
{
    // Allocate enough memory for the maximum possible supported resolution. As
    // of now, it seems like future 8k displays will adopt this resolution.
    milton_state->full_width      = 7680;
    milton_state->full_height     = 4320;
    milton_state->bytes_per_pixel = 4;
    milton_state->view_scale      = (1 << 12);
    milton_state->num_strokes       = 1;  // Working stroke is index 0

    milton_state->gl = arena_alloc_elem(milton_state->root_arena, MiltonGLState);

    // Init picker
    {
        v2i center = { 0 };
        int32 bound_radius_px = 200;
        float wheel_half_width = 30;
        milton_state->picker.bound_radius_px = bound_radius_px;
        milton_state->picker.wheel_half_width = wheel_half_width;
        milton_state->picker.wheel_radius = (float)bound_radius_px - 5.0f - wheel_half_width;
        picker_update(&milton_state->picker,
                (v2i){center.x + (int)(milton_state->picker.wheel_radius), center.y});
    }

    int closest_power_of_two = (1 << 27);  // Ceiling of log2(width * height * bpp)
    milton_state->raster_buffer_size = closest_power_of_two;

    milton_state->raster_buffer = arena_alloc_array(milton_state->root_arena,
            milton_state->raster_buffer_size, uint8);

    milton_gl_backend_init(milton_state);
}

inline v2i canvas_to_raster(v2i screen_size, int32 view_scale, v2i canvas_point)
{
    v2i screen_center = invscale_v2i(screen_size, 2);
    v2i point = canvas_point;
    point = invscale_v2i(point, view_scale);
    point = add_v2i     ( point, screen_center );
    return point;
}

inline v2i raster_to_canvas(v2i screen_size, int32 view_scale, v2i raster_point)
{
    v2i screen_center = invscale_v2i(screen_size, 2);
    v2i canvas_point = raster_point;
    canvas_point = sub_v2i   ( canvas_point ,  screen_center );
    canvas_point = scale_v2i (canvas_point, view_scale);
    return canvas_point;
}

typedef struct BitScanResult_s
{
    uint32 index;
    bool32 found;
} BitScanResult;

inline BitScanResult find_least_significant_set_bit(uint32 value)
{
    BitScanResult result = { 0 };
#if defined(_MSC_VER)
    result.found = _BitScanForward((DWORD*)&result.index, value);
#else
    for (uint32 i = 0; i < 32; ++i)
    {
        if (value & (1 << i))
        {
            result.index = i;
            result.found = true;
            break;
        }
    }
#endif
    return result;
}

inline int32 raster_distance(v2i a, v2i b)
{
    int32 res = maxi(absi(a.x - b.x), absi(a.y - b.y));
    return res;
}

static Rect rect_enlarge(Rect src, int32 offset)
{
    Rect result;
    result.left = src.left - offset;
    result.top = src.top - offset;
    result.right = src.right + offset;
    result.bottom = src.bottom + offset;
    return result;
}

static Rect bounding_rect_for_stroke(v2i points[], int64 num_points)
{
    assert (num_points > 0);

    v2i top_left =  points[0];
    v2i bot_right = points[0];

    for (int64 i = 1; i < num_points; ++i)
    {
        v2i point = points[i];
        if (point.x < top_left.x)   top_left.x = point.x;
        if (point.x > bot_right.x)  bot_right.x = point.x;

        if (point.y < top_left.y)   top_left.y = point.y;
        if (point.y > bot_right.y)  bot_right.y = point.y;
    }
    Rect rect = { top_left, bot_right };
    return rect;
}

inline bool32 is_inside_bounds(v2i point, int32 radius, Rect bounds)
{
    return
        point.x + radius >= bounds.left &&
        point.x - radius <  bounds.right &&
        point.y + radius >= bounds.top &&
        point.y - radius <  bounds.bottom;
}

inline int32 rect_area(Rect rect)
{
    return (rect.right - rect.left) * (rect.bottom - rect.top);
}

static Rect get_stroke_raster_bounds(
        v2i screen_size, int32 view_scale, Stroke* stroke, int32 start, Brush brush)
{
    v2i* points = stroke->points;
    int32 num_points = stroke->num_points;
    v2i point = canvas_to_raster(screen_size, view_scale, points[0]);

    Rect limits = { point.x, point.y, point.x, point.y };
    assert ( start < stroke->num_points );
    for (int i = start; i < num_points; ++i)
    {
        v2i point = canvas_to_raster(screen_size, view_scale, points[i]);
        if (point.x < limits.left)
            limits.left = point.x;
        if (point.x > limits.right)
            limits.right = point.x;
        if (point.y < limits.top)
            limits.top = point.y;
        if (point.y > limits.bottom)
            limits.bottom = point.y;
    }
    limits = rect_enlarge(limits, (brush.radius / view_scale));

    assert (limits.right >= limits.left);
    assert (limits.bottom >= limits.top);
    return limits;
}

static void stroke_clip_to_rect(Stroke* stroke, Rect rect)
{
    stroke->num_clipped_points = 0;
    if (stroke->num_points == 1)
    {
        if (is_inside_bounds(stroke->points[0], stroke->brush.radius, rect))
        {
            stroke->clipped_points[stroke->num_clipped_points++] = stroke->points[0];
        }
    }
    else
    {
        for (int32 point_i = 0; point_i < stroke->num_points - 1; ++point_i)
        {
            v2i a = stroke->points[point_i];
            v2i b = stroke->points[point_i + 1];

            // Very conservative...
            bool32 inside =
                !(
                        (a.x > rect.right && b.x > rect.right) ||
                        (a.x < rect.left && b.x < rect.left) ||
                        (a.y < rect.top && b.y < rect.top) ||
                        (a.y > rect.bottom && b.y > rect.bottom)
                 );

            // We can add the segment
            if (inside)
            {
                stroke->clipped_points[stroke->num_clipped_points++] = a;
                stroke->clipped_points[stroke->num_clipped_points++] = b;
            }
        }
    }
}

// This actually makes things faster in render_rect.
typedef struct LinkedList_Stroke_s
{
    Stroke* elem;
    struct LinkedList_Stroke_s* next;
} LinkedList_Stroke;

static void render_rect(MiltonState* milton_state, Rect limits)
{
    static uint32 mask_a = 0xff000000;
    static uint32 mask_r = 0x00ff0000;
    static uint32 mask_g = 0x0000ff00;
    static uint32 mask_b = 0x000000ff;
    uint32 shift_a = find_least_significant_set_bit(mask_a).index;
    uint32 shift_r = find_least_significant_set_bit(mask_r).index;
    uint32 shift_g = find_least_significant_set_bit(mask_g).index;
    uint32 shift_b = find_least_significant_set_bit(mask_b).index;

    uint32* pixels = (uint32*)milton_state->raster_buffer;
    Stroke* strokes = milton_state->strokes;

    Rect canvas_limits;
    canvas_limits.top_left = raster_to_canvas(milton_state->screen_size, milton_state->view_scale,
            limits.top_left);
    canvas_limits.bot_right = raster_to_canvas(milton_state->screen_size, milton_state->view_scale,
            limits.bot_right);

    LinkedList_Stroke* stroke_list = NULL;

    // Go backwards so that list is in the correct older->newer order.
    for (int stroke_i = milton_state->num_strokes - 1; stroke_i >= 0; --stroke_i)
    {
        Stroke* stroke = &strokes[stroke_i];
        Rect enlarged_limits = rect_enlarge(canvas_limits, stroke->brush.radius);
        stroke_clip_to_rect(stroke, enlarged_limits);
        if (stroke->num_clipped_points)
        {
            LinkedList_Stroke* list_elem = arena_alloc_elem(
                    milton_state->transient_arena, LinkedList_Stroke);

            LinkedList_Stroke* tail = stroke_list;
            list_elem->elem = stroke;
            list_elem->next = stroke_list;
            stroke_list = list_elem;
        }
    }

    for (int j = limits.top; j < limits.bottom; ++j)
    {
        float f = ((float)j / milton_state->screen_size.h);

        for (int i = limits.left; i < limits.right; ++i)
        {
            v2i raster_point = {i, j};
            v2i canvas_point = raster_to_canvas(
                    milton_state->screen_size, milton_state->view_scale, raster_point);

            // Clear color
#if 1
            float dr = 1.0f;
            float dg = 1.0f;
            float db = 1.0f;
#else
            float dr = f;
            float dg = f;
            float db = f;
#endif
            float da = 1.0f;

            struct LinkedList_Stroke_s* list_iter = stroke_list;
            while(list_iter)
            {
                Stroke* stroke = list_iter->elem;

                assert (stroke);
                v2i* points = stroke->clipped_points;

                v2i min_point = {0};
                float min_dist = FLT_MAX;
                float dx = 0;
                float dy = 0;
                //int64 radius_squared = stroke->brush.radius * stroke->brush.radius;
                if (stroke->num_clipped_points == 1)
                {
                    min_point = points[0];
                    dx = (float) (canvas_point.x - min_point.x);
                    dy = (float) (canvas_point.y - min_point.y);
                    min_dist = dx * dx + dy * dy;
                }
                else
                {
                    for (int point_i = 0; point_i < stroke->num_clipped_points - 1; point_i += 2)
                    {
                        // Find closest point.
                        v2i a = points[point_i];
                        v2i b = points[point_i + 1];

                        v2f ab = {(float)b.x - a.x, (float)b.y - a.y};
                        float mag_ab2 = ab.x * ab.x + ab.y * ab.y;
                        if (mag_ab2 > 0)
                        {
                            float mag_ab = sqrtf(mag_ab2);
                            float d_x = ab.x / mag_ab;
                            float d_y = ab.y / mag_ab;
                            float ax_x = (float)canvas_point.x - a.x;
                            float ax_y = (float)canvas_point.y - a.y;
                            float disc = d_x * ax_x + d_y * ax_y;
                            v2i point;
                            if (disc >= 0 && disc <= mag_ab)
                            {
                                point = (v2i)
                                {
                                    (int32)(a.x + disc * d_x), (int32)(a.y + disc * d_y),
                                };
                            }
                            else if (disc < 0)
                            {
                                point = a;
                            }
                            else
                            {
                                point = b;
                            }
                            float test_dx = (float) (canvas_point.x - point.x);
                            float test_dy = (float) (canvas_point.y - point.y);
                            float dist = test_dx * test_dx + test_dy * test_dy;
                            if (dist < min_dist)
                            {
                                min_dist = dist;
                                min_point = point;
                                dx = test_dx;
                                dy = test_dy;
                            }
                        }
                    }
                }


                if (min_dist < FLT_MAX)
                {
                    int samples = 0;
                    {
                        float u = 0.223607f * milton_state->view_scale;  // sin(arctan(1/2)) / 2
                        float v = 0.670820f * milton_state->view_scale;  // cos(arctan(1/2)) / 2 + u

                        float dists[4];
                        dists[0] = (dx - u) * (dx - u) + (dy - v) * (dy - v);
                        dists[1] = (dx - v) * (dx - v) + (dy + u) * (dy + u);
                        dists[2] = (dx + u) * (dx + u) + (dy + v) * (dy + v);
                        dists[3] = (dx + v) * (dx + v) + (dy + u) * (dy + u);
                        for (int i = 0; i < 4; ++i)
                        {
                            if (sqrtf(dists[i]) < stroke->brush.radius)
                            {
                                ++samples;
                            }
                        }
                    }

                    // If the stroke contributes to the pixel, do compositing.
                    if (samples > 0)
                    {
                        // Do compositing
                        // ---------------

                        float coverage = (float)samples / 4.0f;

                        float sr = stroke->brush.color.r;
                        float sg = stroke->brush.color.g;
                        float sb = stroke->brush.color.b;
                        float sa = stroke->brush.alpha;

                        sa *= coverage;

                        dr = (1 - sa) * dr + sa * sr;
                        dg = (1 - sa) * dg + sa * sg;
                        db = (1 - sa) * db + sa * sb;
                        da = sa + da * (1 - sa);
                    }
                }

                list_iter = list_iter->next;

            }
            // From [0, 1] to [0, 255]
            uint32 pixel =
                ((uint8)(dr * 255.0f) << shift_r) +
                ((uint8)(dg * 255.0f) << shift_g) +
                ((uint8)(db * 255.0f) << shift_b) +
                ((uint8)(da * 255.0f) << shift_a);

            pixels[j * milton_state->screen_size.w + i] = pixel;
        }
    }
}

static int32 rect_split(Arena* transient_arena,
        Rect src_rect, int32 width, int32 height, Rect** dest_rects)
{
    int n_width = (src_rect.right - src_rect.left) / width;
    int n_height = (src_rect.bottom - src_rect.top) / height;

    if (!n_width || !n_height)
    {
        return 0;
    }

    int32 num_rects = (n_width + 1) * (n_height + 1);
    *dest_rects = arena_alloc_array(transient_arena, num_rects, Rect);

    int32 i = 0;
    for (int h = src_rect.top; h < src_rect.bottom; h += height)
    {
        for (int w = src_rect.left; w < src_rect.right; w += width)
        {
            Rect rect;
            {
                rect.left = w;
                rect.right = min(src_rect.right, w + width);
                rect.top = h;
                rect.bottom = min(src_rect.bottom, h + height);
            }
            (*dest_rects)[i++] = rect;
        }
    }
    assert(i <= num_rects);
    return i;
}

inline bool32 is_user_drawing(MiltonState* milton_state)
{
    bool32 result = milton_state->working_stroke.num_points > 0;
    return result;
}

// Returns non-zero if the raster buffer was modified by this update.
static bool32 milton_update(MiltonState* milton_state, MiltonInput* input)
{
    arena_reset(milton_state->transient_arena);
    bool32 updated = false;

    if (input->scale)
    {
        input->full_refresh = true;
        static float scale_factor = 1.5f;
        static int32 view_scale_limit = 1900000;
        if (input->scale > 0 && milton_state->view_scale > 2)
        {
            milton_state->view_scale = (int32)(milton_state->view_scale / scale_factor);
        }
        else if (milton_state->view_scale < view_scale_limit)
        {
            milton_state->view_scale = (int32)(milton_state->view_scale * scale_factor) + 1;
        }
    }

    if (input->reset)
    {
        milton_state->num_strokes = 1;
        milton_state->strokes[0].num_points = 0;
        milton_state->working_stroke.num_points = 0;
        input->full_refresh = true;
    }

    Brush brush = { 0 };
    {
        brush.radius = 10 * milton_state->view_scale;
        brush.alpha = 0.5f;
        brush.color = (v3f){ 0.7f, 0.5f, 0.7f };
    }

    bool32 finish_stroke = false;
    if (input->point)
    {
        v2i point = *input->point;
        if (!is_user_drawing(milton_state) && is_inside_picker(&milton_state->picker, point))
        {
            ColorPickResult pick_result = picker_update(&milton_state->picker, point);
            switch (pick_result)
            {
            case ColorPickResult_change_color:
                {
                    // brush.color = something.
                }
            case ColorPickResult_redraw_picker:
                {
                    break;
                }
            case ColorPickResult_nothing:
                {
                    break;
                }
            }

        }
        else
        {
            v2i in_point = *input->point;

            // Avoid creating really large update rects when starting. new strokes
            if (milton_state->working_stroke.num_points == 0)
            {
                milton_state->last_point = in_point;
            }

            v2i canvas_point = raster_to_canvas(milton_state->screen_size, milton_state->view_scale, in_point);

            // TODO: make deque!!
            if (milton_state->working_stroke.num_points < LIMIT_STROKE_POINTS)
            {
                // Add to current stroke.
                milton_state->working_stroke.points[milton_state->working_stroke.num_points++] = canvas_point;
                milton_state->working_stroke.brush = brush;
                milton_state->working_stroke.bounds =
                    bounding_rect_for_stroke(milton_state->working_stroke.points,
                            milton_state->working_stroke.num_points);

            }
            milton_state->strokes[0] = milton_state->working_stroke;  // Copy current stroke.

            milton_state->last_point = in_point;

            updated = true;
        }
    }
    if (input->end_stroke)
    {
        if (milton_state->num_strokes < 4096)
        {
            // Copy current stroke.
            milton_state->strokes[milton_state->num_strokes++] = milton_state->working_stroke;
            // Clear working_stroke
            {
                milton_state->strokes[0] = (Stroke){ 0 };  // Clear working stroke.
                milton_state->working_stroke.num_points = 0;
            }
        }
    }

    Rect limits = { 0 };

    if (input->full_refresh)
    {
        limits.left = 0;
        limits.right = milton_state->screen_size.w;
        limits.top = 0;
        limits.bottom = milton_state->screen_size.h;
        Rect* split_rects = NULL;
        int32 num_rects = rect_split(milton_state->transient_arena,
                limits, 30, 30, &split_rects);
        for (int i = 0; i < num_rects; ++i)
        {
            render_rect(milton_state, split_rects[i]);
        }
    }
    else if (milton_state->strokes[0].num_points > 1)
    {
        Stroke* stroke = &milton_state->strokes[0];
        v2i new_point = canvas_to_raster(
                    milton_state->screen_size, milton_state->view_scale,
                    stroke->points[stroke->num_points - 2]);

        limits.left =   min (milton_state->last_point.x, new_point.x);
        limits.right =  max (milton_state->last_point.x, new_point.x);
        limits.top =    min (milton_state->last_point.y, new_point.y);
        limits.bottom = max (milton_state->last_point.y, new_point.y);
        limits = rect_enlarge(limits, (stroke->brush.radius / milton_state->view_scale));

        render_rect(milton_state, limits);
    }
    else if (milton_state->strokes[0].num_points == 1)
    {
        Stroke* stroke = &milton_state->strokes[0];
        v2i point = canvas_to_raster(milton_state->screen_size, milton_state->view_scale,
                stroke->points[0]);
        int32 raster_radius = stroke->brush.radius / milton_state->view_scale;
        limits.left = -raster_radius  + point.x;
        limits.right = raster_radius  + point.x;
        limits.top = -raster_radius   + point.y;
        limits.bottom = raster_radius + point.y;
        render_rect(milton_state, limits);
    }
    updated = true;

    return updated;
}
