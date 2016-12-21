#ifndef slic3r_Surface_hpp_
#define slic3r_Surface_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"

namespace Slic3r {

enum SurfaceType { 
    // Top horizontal surface, visible from the top.
    stTop,
    // Bottom horizontal surface, visible from the bottom, printed with a normal extrusion flow.
    stBottom,
    // Bottom horizontal surface, visible from the bottom, unsupported, printed with a bridging extrusion flow.
    stBottomBridge,
    // Normal sparse infill.
    stInternal,
    // Full infill, supporting the top surfaces and/or defining the verticall wall thickness.
    stInternalSolid,
    // 1st layer of dense infill over sparse infill, printed with a bridging extrusion flow.
    stInternalBridge,
    // stInternal turns into void surfaces if the sparse infill is used for supports only,
    // or if sparse infill layers get combined into a single layer.
    stInternalVoid,
    // Inner/outer perimeters.
    stPerimeter
};

class Surface
{
public:
    SurfaceType     surface_type;
    ExPolygon       expolygon;
    double          thickness;          // in mm
    unsigned short  thickness_layers;   // in layers
    double          bridge_angle;       // in radians, ccw, 0 = East, only 0+ (negative means undefined)
    unsigned short  extra_perimeters;
    
    Surface(SurfaceType _surface_type, const ExPolygon &_expolygon)
        : surface_type(_surface_type), expolygon(_expolygon),
            thickness(-1), thickness_layers(1), bridge_angle(-1), extra_perimeters(0)
        {};
    Surface(const Surface &other, const ExPolygon &_expolygon)
        : surface_type(other.surface_type), expolygon(_expolygon),
            thickness(other.thickness), thickness_layers(other.thickness_layers), bridge_angle(other.bridge_angle), extra_perimeters(other.extra_perimeters)
        {};
#if SLIC3R_CPPVER >= 11
    Surface(SurfaceType _surface_type, const ExPolygon &&_expolygon)
        : surface_type(_surface_type), expolygon(std::move(_expolygon)),
            thickness(-1), thickness_layers(1), bridge_angle(-1), extra_perimeters(0)
        {};
    Surface(const Surface &other, const ExPolygon &&_expolygon)
        : surface_type(other.surface_type), expolygon(std::move(_expolygon)),
            thickness(other.thickness), thickness_layers(other.thickness_layers), bridge_angle(other.bridge_angle), extra_perimeters(other.extra_perimeters)
        {};
#endif
    operator Polygons() const;
    double area() const;
    bool empty() const { return expolygon.empty(); }
    bool is_solid() const;
    bool is_external() const;
    bool is_internal() const;
    bool is_bottom() const;
    bool is_bridge() const;
};

typedef std::vector<Surface> Surfaces;
typedef std::vector<Surface*> SurfacesPtr;

inline Polygons to_polygons(const Surfaces &src)
{
    size_t num = 0;
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it)
        num += it->expolygon.holes.size() + 1;
    Polygons polygons;
    polygons.reserve(num);
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it) {
        polygons.push_back(it->expolygon.contour);
        for (Polygons::const_iterator ith = it->expolygon.holes.begin(); ith != it->expolygon.holes.end(); ++ith)
            polygons.push_back(*ith);
    }
    return polygons;
}

inline Polygons to_polygons(const SurfacesPtr &src)
{
    size_t num = 0;
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++it)
        num += (*it)->expolygon.holes.size() + 1;
    Polygons polygons;
    polygons.reserve(num);
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++it) {
        polygons.push_back((*it)->expolygon.contour);
        for (Polygons::const_iterator ith = (*it)->expolygon.holes.begin(); ith != (*it)->expolygon.holes.end(); ++ith)
            polygons.push_back(*ith);
    }
    return polygons;
}

inline ExPolygons to_expolygons(const Surfaces &src)
{
    ExPolygons expolygons;
    expolygons.reserve(src.size());
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++it)
        expolygons.push_back(it->expolygon);
    return expolygons;
}

inline ExPolygons to_expolygons(const SurfacesPtr &src)
{
    ExPolygons expolygons;
    expolygons.reserve(src.size());
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++it)
        expolygons.push_back((*it)->expolygon);
    return expolygons;
}

// Count a nuber of polygons stored inside the vector of expolygons.
// Useful for allocating space for polygons when converting expolygons to polygons.
inline size_t number_polygons(const Surfaces &surfaces)
{
    size_t n_polygons = 0;
    for (Surfaces::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        n_polygons += it->expolygon.holes.size() + 1;
    return n_polygons;
}
inline size_t number_polygons(const SurfacesPtr &surfaces)
{
    size_t n_polygons = 0;
    for (SurfacesPtr::const_iterator it = surfaces.begin(); it != surfaces.end(); ++ it)
        n_polygons += (*it)->expolygon.holes.size() + 1;
    return n_polygons;
}

// Append a vector of Surfaces at the end of another vector of polygons.
inline void polygons_append(Polygons &dst, const Surfaces &src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back(it->expolygon.contour);
        dst.insert(dst.end(), it->expolygon.holes.begin(), it->expolygon.holes.end());
    }
}

#if SLIC3R_CPPVER >= 11
inline void polygons_append(Polygons &dst, Surfaces &&src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (Surfaces::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back(std::move(it->expolygon.contour));
        std::move(std::begin(it->expolygon.holes), std::end(it->expolygon.holes), std::back_inserter(dst));
    }
}
#endif

// Append a vector of Surfaces at the end of another vector of polygons.
inline void polygons_append(Polygons &dst, const SurfacesPtr &src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back((*it)->expolygon.contour);
        dst.insert(dst.end(), (*it)->expolygon.holes.begin(), (*it)->expolygon.holes.end());
    }
}

#if SLIC3R_CPPVER >= 11
inline void polygons_append(Polygons &dst, SurfacesPtr &&src) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (SurfacesPtr::const_iterator it = src.begin(); it != src.end(); ++ it) {
        dst.push_back(std::move((*it)->expolygon.contour));
        std::move(std::begin((*it)->expolygon.holes), std::end((*it)->expolygon.holes), std::back_inserter(dst));
    }
}
#endif

// Append a vector of Surfaces at the end of another vector of polygons.
inline void surfaces_append(Surfaces &dst, const ExPolygons &src, SurfaceType surfaceType) 
{ 
    dst.reserve(dst.size() + src.size());
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++ it)
        dst.push_back(Surface(surfaceType, *it));
}
inline void surfaces_append(Surfaces &dst, const ExPolygons &src, const Surface &surfaceTempl) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++ it)
        dst.push_back(Surface(surfaceTempl, *it));
}
inline void surfaces_append(Surfaces &dst, const Surfaces &src) 
{ 
    dst.insert(dst.end(), src.begin(), src.end());
}

#if SLIC3R_CPPVER >= 11
inline void surfaces_append(Surfaces &dst, ExPolygons &&src, SurfaceType surfaceType) 
{ 
    dst.reserve(dst.size() + src.size());
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++ it)
        dst.push_back(Surface(surfaceType, std::move(*it)));
}
inline void surfaces_append(Surfaces &dst, ExPolygons &&src, const Surface &surfaceTempl) 
{ 
    dst.reserve(dst.size() + number_polygons(src));
    for (ExPolygons::const_iterator it = src.begin(); it != src.end(); ++ it)
        dst.push_back(Surface(surfaceTempl, std::move(*it)));
}
inline void surfaces_append(Surfaces &dst, Surfaces &&src) 
{ 
    if (dst.empty())
        dst = std::move(src);
    else
        std::move(std::begin(src), std::end(src), std::back_inserter(dst));
}
#endif

extern BoundingBox get_extents(const Surface &surface);
extern BoundingBox get_extents(const Surfaces &surfaces);
extern BoundingBox get_extents(const SurfacesPtr &surfaces);

inline bool surfaces_could_merge(const Surface &s1, const Surface &s2)
{
    return 
        s1.surface_type      == s2.surface_type     &&
        s1.thickness         == s2.thickness        &&
        s1.thickness_layers  == s2.thickness_layers &&
        s1.bridge_angle      == s2.bridge_angle;
}

class SVG;

extern const char* surface_type_to_color_name(const SurfaceType surface_type);
extern void export_surface_type_legend_to_svg(SVG &svg, const Point &pos);
extern Point export_surface_type_legend_to_svg_box_size();
extern bool export_to_svg(const char *path, const Surfaces &surfaces, const float transparency = 1.f);

}

#endif
