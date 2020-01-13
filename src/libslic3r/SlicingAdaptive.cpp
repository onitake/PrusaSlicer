#include "libslic3r.h"
#include "TriangleMesh.hpp"
#include "SlicingAdaptive.hpp"

namespace Slic3r
{

void SlicingAdaptive::clear()
{
	m_meshes.clear();
	m_faces.clear();
	m_face_normal_z.clear();
}

std::pair<float, float> face_z_span(const stl_facet *f)
{
	return std::pair<float, float>(
		std::min(std::min(f->vertex[0](2), f->vertex[1](2)), f->vertex[2](2)),
		std::max(std::max(f->vertex[0](2), f->vertex[1](2)), f->vertex[2](2)));
}

void SlicingAdaptive::prepare()
{
	// 1) Collect faces of all meshes.
	int nfaces_total = 0;
	for (std::vector<const TriangleMesh*>::const_iterator it_mesh = m_meshes.begin(); it_mesh != m_meshes.end(); ++ it_mesh)
		nfaces_total += (*it_mesh)->stl.stats.number_of_facets;
	m_faces.reserve(nfaces_total);
	for (std::vector<const TriangleMesh*>::const_iterator it_mesh = m_meshes.begin(); it_mesh != m_meshes.end(); ++ it_mesh)
		for (const stl_facet &face : (*it_mesh)->stl.facet_start)
			m_faces.emplace_back(&face);

	// 2) Sort faces lexicographically by their Z span.
	std::sort(m_faces.begin(), m_faces.end(), [](const stl_facet *f1, const stl_facet *f2) {
		std::pair<float, float> span1 = face_z_span(f1);
		std::pair<float, float> span2 = face_z_span(f2);
		return span1 < span2;
	});

	// 3) Generate Z components of the facet normals.
	m_face_normal_z.assign(m_faces.size(), 0.f);
    for (size_t iface = 0; iface < m_faces.size(); ++ iface)
    	m_face_normal_z[iface] = m_faces[iface]->normal(2);
}

float SlicingAdaptive::cusp_height(float z, float cusp_value, int &current_facet)
{
	float height = m_slicing_params.max_layer_height;
	bool first_hit = false;
	
	// find all facets intersecting the slice-layer
	int ordered_id = current_facet;
	for (; ordered_id < int(m_faces.size()); ++ ordered_id) {
		std::pair<float, float> zspan = face_z_span(m_faces[ordered_id]);
		// facet's minimum is higher than slice_z -> end loop
		if (zspan.first >= z)
			break;
		// facet's maximum is higher than slice_z -> store the first event for next cusp_height call to begin at this point
		if (zspan.second > z) {
			// first event?
			if (! first_hit) {
				first_hit = true;
				current_facet = ordered_id;
			}
			// skip touching facets which could otherwise cause small cusp values
			if (zspan.second <= z + EPSILON)
				continue;
			// compute cusp-height for this facet and store minimum of all heights
			float normal_z = m_face_normal_z[ordered_id];
			height = std::min(height, (normal_z == 0.f) ? 9999.f : std::abs(cusp_value / normal_z));
		}
	}

	// lower height limit due to printer capabilities
	height = std::max(height, float(m_slicing_params.min_layer_height));

	// check for sloped facets inside the determined layer and correct height if necessary
	if (height > m_slicing_params.min_layer_height) {
		for (; ordered_id < int(m_faces.size()); ++ ordered_id) {
			std::pair<float, float> zspan = face_z_span(m_faces[ordered_id]);
			// facet's minimum is higher than slice_z + height -> end loop
			if (zspan.first >= z + height)
				break;

			// skip touching facets which could otherwise cause small cusp values
			if (zspan.second <= z + EPSILON)
				continue;

			// Compute cusp-height for this facet and check against height.
			float normal_z = m_face_normal_z[ordered_id];
			float cusp = (normal_z == 0) ? 9999 : abs(cusp_value / normal_z);
			
			float z_diff = zspan.first - z;

			// handle horizontal facets
			if (m_face_normal_z[ordered_id] > 0.999) {
				// Slic3r::debugf "cusp computation, height is reduced from %f", $height;
				height = z_diff;
				// Slic3r::debugf "to %f due to near horizontal facet\n", $height;
			} else if (cusp > z_diff) {
				if (cusp < height) {
					// Slic3r::debugf "cusp computation, height is reduced from %f", $height;
					height = cusp;
					// Slic3r::debugf "to %f due to new cusp height\n", $height;
				}
			} else {
				// Slic3r::debugf "cusp computation, height is reduced from %f", $height;
				height = z_diff;
				// Slic3r::debugf "to z-diff: %f\n", $height;
			}
		}
		// lower height limit due to printer capabilities again
		height = std::max(height, float(m_slicing_params.min_layer_height));
	}
	
//	Slic3r::debugf "cusp computation, layer-bottom at z:%f, cusp_value:%f, resulting layer height:%f\n", unscale $z, $cusp_value, $height;	
	return height; 
}

// Returns the distance to the next horizontal facet in Z-dir 
// to consider horizontal object features in slice thickness
float SlicingAdaptive::horizontal_facet_distance(float z)
{
	for (size_t i = 0; i < m_faces.size(); ++ i) {
		std::pair<float, float> zspan = face_z_span(m_faces[i]);
		// facet's minimum is higher than max forward distance -> end loop
		if (zspan.first > z + m_slicing_params.max_layer_height)
			break;
		// min_z == max_z -> horizontal facet
		if (zspan.first > z && zspan.first == zspan.second)
			return zspan.first - z;
	}
	
	// objects maximum?
	return (z + m_slicing_params.max_layer_height > m_slicing_params.object_print_z_height()) ? 
		std::max<float>(m_slicing_params.object_print_z_height() - z, 0.f) :
		m_slicing_params.max_layer_height;
}

}; // namespace Slic3r
