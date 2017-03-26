// Copyright (c) 2015-2016 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license


#pragma once

#include "common.h"

template<typename T>
struct Vector2
{
    union {
        struct {
            T x;
            T y;
        };
        struct {
            T w;
            T h;
        };
        T d[2];
        T xy[2];
    };
};

// Types
typedef Vector2<i32>     v2i;
typedef Vector2<i64>     v2ii;
typedef Vector2<float>   v2f;
#define operator2_scalar(OP) \
        template<typename T> \
        Vector2<T> operator OP (const Vector2<T>& v, T f) \
{                       \
                        \
    Vector2<T> r = {    \
        v.x * f,        \
        v.y * f,        \
    };                  \
    return r;           \
}
#define operator2_vector(OP) \
        template<typename T> \
        Vector2<T> operator OP (const Vector2<T>& a, const Vector2<T>& b) \
{                           \
                            \
    Vector2<T> r = {        \
        a.x  OP b.x,        \
        a.y  OP b.y,        \
    };                      \
    return r;               \
}

operator2_vector(+)
operator2_vector(-)
operator2_scalar(*)
operator2_scalar(/)

template<typename Type>
bool
operator == (const Type& a, const Type& b)
{
    return a.x == b.x && a.y == b.y;
}

template<typename Type>
Vector2<Type>
perpendicular(const Vector2<Type> &v)
{
    Vector2<Type> r = {
        -v.y,
        v.x
    };
    return r;
}

v2f lerp(v2f a, v2f b, float t);



#if defined(_WIN32)
#pragma warning(push)
#pragma warning(disable:4587) // Constructor for xyz not implicitly called
#endif
template<typename T>
struct Vector3
{
    union {
        struct {
            T x;
            T y;
            T z;
        };
        struct {
            T r;
            T g;
            T b;
        };
        struct {
            T h;
            T s;
            T v;
        };

        struct {
            Vector2<T> xy;
            T z_;
        };
        T d[3];
        T xyz[3];
        T hsv[3];
    };
};
#if defined(_WIN32)
#pragma warning(pop)
#endif

// Types
typedef Vector3<int>     v3i;
typedef Vector3<float>   v3f;


template<typename T>
struct Vector4
{
    union {
        struct {
            T x;
            T y;
            T z;
            T w;
        };
        struct {
            T r;
            T g;
            T b;
            T a;
        };
        T d[4];
        struct {

            Vector3<T> xyz;
            float w_;
        };
        struct {
            Vector2<T> xy;
            Vector2<T> zw;
        };
        struct {

            Vector3<T> rgb;
            float a_;
        };

        T xyzw[4];
        T rgba[4];
    };
};

typedef Vector4<float>  v4f;
typedef Vector4<i32>    v4i;

