#include "assets/ObjLoader.h"

#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace
{
    // Parse one OBJ face-vertex token ("v", "v/vt", "v/vt/vn", "v//vn"). Indices are 1-based in the
    // file; negative means relative-to-end. Returns 0-based v/vn (vn = -1 if absent).
    void ParseFaceVert(const std::string& tok, int vCount, int vnCount, int& vOut, int& vnOut)
    {
        int v = 0, vt = 0, vn = 0;
        int field = 0;
        bool any = false;
        int sign = 1, val = 0;
        auto flush = [&]() {
            int s = sign * val;
            if (field == 0) v = s;
            else if (field == 1) vt = s;
            else if (field == 2) vn = s;
            sign = 1; val = 0; any = false;
        };
        for (char c : tok)
        {
            if (c == '/') { if (any) flush(); else { sign = 1; val = 0; } field++; }
            else if (c == '-') sign = -1;
            else if (c >= '0' && c <= '9') { val = val * 10 + (c - '0'); any = true; }
        }
        if (any) flush();

        vOut  = (v  > 0) ? v  - 1 : (v  < 0 ? vCount  + v  : 0);
        vnOut = (vn > 0) ? vn - 1 : (vn < 0 ? vnCount + vn : -1);
        (void)vt;
    }
}

bool LoadObj(const std::string& path,
             std::vector<float>&    outPositions,
             std::vector<float>&    outNormals,
             std::vector<uint32_t>& outIndices)
{
    outPositions.clear();
    outNormals.clear();
    outIndices.clear();

    std::ifstream f(path);
    if (!f.is_open())
    {
        fprintf(stderr, "[obj] cannot open %s\n", path.c_str());
        return false;
    }

    std::vector<float> vs;   // raw positions (3/vert)
    std::vector<float> vns;  // raw normals   (3/vert)
    // Each face corner as (vIndex, vnIndex); triangulated fan indices reference these.
    std::vector<std::pair<int, int>> corners;
    std::vector<uint32_t> cornerTris;  // indices into `corners`, 3 per triangle

    std::string line;
    while (std::getline(f, line))
    {
        if (line.size() < 2) continue;
        if (line[0] == 'v' && line[1] == ' ')
        {
            std::istringstream s(line.substr(2));
            float x, y, z; s >> x >> y >> z;
            vs.push_back(x); vs.push_back(y); vs.push_back(z);
        }
        else if (line[0] == 'v' && line[1] == 'n')
        {
            std::istringstream s(line.substr(3));
            float x, y, z; s >> x >> y >> z;
            vns.push_back(x); vns.push_back(y); vns.push_back(z);
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            std::istringstream s(line.substr(2));
            std::string tok;
            std::vector<uint32_t> poly;  // corner indices for this face
            int vCount = (int)(vs.size() / 3), vnCount = (int)(vns.size() / 3);
            while (s >> tok)
            {
                int vi, vni;
                ParseFaceVert(tok, vCount, vnCount, vi, vni);
                poly.push_back((uint32_t)corners.size());
                corners.emplace_back(vi, vni);
            }
            // Fan-triangulate the polygon.
            for (size_t i = 2; i < poly.size(); ++i)
            {
                cornerTris.push_back(poly[0]);
                cornerTris.push_back(poly[i - 1]);
                cornerTris.push_back(poly[i]);
            }
        }
    }

    if (cornerTris.empty() || vs.empty())
    {
        fprintf(stderr, "[obj] %s has no faces/vertices\n", path.c_str());
        return false;
    }

    const bool haveNormals = !vns.empty();

    if (haveNormals)
    {
        // De-duplicate (v, vn) corners into unique vertices.
        std::unordered_map<uint64_t, uint32_t> remap;
        remap.reserve(corners.size());
        auto emit = [&](uint32_t cornerIdx) -> uint32_t {
            const auto& c = corners[cornerIdx];
            int vi = c.first, vni = c.second;
            uint64_t key = ((uint64_t)(uint32_t)vi << 32) | (uint32_t)(vni + 1);
            auto it = remap.find(key);
            if (it != remap.end()) return it->second;
            uint32_t idx = (uint32_t)(outPositions.size() / 3);
            outPositions.push_back(vs[vi * 3 + 0]);
            outPositions.push_back(vs[vi * 3 + 1]);
            outPositions.push_back(vs[vi * 3 + 2]);
            if (vni >= 0)
            {
                outNormals.push_back(vns[vni * 3 + 0]);
                outNormals.push_back(vns[vni * 3 + 1]);
                outNormals.push_back(vns[vni * 3 + 2]);
            }
            else { outNormals.push_back(0); outNormals.push_back(1); outNormals.push_back(0); }
            remap.emplace(key, idx);
            return idx;
        };
        for (uint32_t ci : cornerTris)
            outIndices.push_back(emit(ci));
    }
    else
    {
        // One vertex per position; accumulate face normals then normalize.
        size_t vcount = vs.size() / 3;
        outPositions = vs;
        outNormals.assign(vcount * 3, 0.0f);
        for (size_t t = 0; t < cornerTris.size(); t += 3)
        {
            int a = corners[cornerTris[t + 0]].first;
            int b = corners[cornerTris[t + 1]].first;
            int c = corners[cornerTris[t + 2]].first;
            float ax = vs[a*3], ay = vs[a*3+1], az = vs[a*3+2];
            float bx = vs[b*3], by = vs[b*3+1], bz = vs[b*3+2];
            float cx = vs[c*3], cy = vs[c*3+1], cz = vs[c*3+2];
            float ux = bx-ax, uy = by-ay, uz = bz-az;
            float wx = cx-ax, wy = cy-ay, wz = cz-az;
            float nx = uy*wz - uz*wy, ny = uz*wx - ux*wz, nz = ux*wy - uy*wx;
            int tri[3] = { a, b, c };
            for (int k = 0; k < 3; ++k)
            {
                outNormals[tri[k]*3+0] += nx;
                outNormals[tri[k]*3+1] += ny;
                outNormals[tri[k]*3+2] += nz;
            }
            outIndices.push_back((uint32_t)a);
            outIndices.push_back((uint32_t)b);
            outIndices.push_back((uint32_t)c);
        }
        for (size_t i = 0; i < vcount; ++i)
        {
            float* n = &outNormals[i*3];
            float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (len > 1e-8f) { n[0]/=len; n[1]/=len; n[2]/=len; }
            else { n[0]=0; n[1]=1; n[2]=0; }
        }
    }

    fprintf(stderr, "[obj] loaded %s: %zu verts, %zu tris\n",
            path.c_str(), outPositions.size()/3, outIndices.size()/3);
    return true;
}
