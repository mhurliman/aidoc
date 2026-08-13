#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal Wavefront OBJ loader for static props. Reads positions (v) and, when present, normals
// (vn); texture coords are ignored. Polygons are triangulated as fans. Face-vertex tuples are
// de-duplicated into an indexed mesh. If the file has no vn records, smooth per-vertex normals are
// computed from face geometry.
//
// Outputs: positions/normals are 3 floats per vertex (parallel arrays); indices are 3 per triangle.
// Returns false (and leaves outputs empty) if the file can't be opened or has no faces.
bool LoadObj(const std::string& path,
             std::vector<float>&    outPositions,
             std::vector<float>&    outNormals,
             std::vector<uint32_t>& outIndices);
