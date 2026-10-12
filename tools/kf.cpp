#include "kf.h"

#include <algorithm>
#include <cmath>

namespace kf
{
	namespace
	{
		const uint32_t kNone = 0xFFFFFFFF;

		bool Valid(float v) { return std::fabs(v) < 1e30f; }   // the NIF marks an unset value with FLT_MAX

		std::string Str(const nif::File& f, uint32_t idx) { return idx < f.strings.size() ? f.strings[idx] : std::string(); }

		Quat ReadQuat(nif::Reader& r)
		{
			Quat q;
			q.w = r.f32(); q.x = r.f32(); q.y = r.f32(); q.z = r.f32();
			return q;
		}

		void ReadPose(nif::Reader& r, Pose& p)
		{
			p.translation = r.v3();
			p.rotation = ReadQuat(r);
			p.scale = r.f32();
			p.hasTranslation = Valid(p.translation.x);
			p.hasRotation = Valid(p.rotation.w);
			p.hasScale = Valid(p.scale);
		}

		// A KeyGroup<float>: count, then (if any) the interpolation and the keys.
		void ReadFloatKeys(nif::Reader& r, std::vector<Track::FKey>& out)
		{
			uint32_t n = r.u32();
			if (!n) return;
			uint32_t type = r.u32();
			for (uint32_t i = 0; i < n; i++) {
				Track::FKey k;
				k.t = r.f32();
				k.v = r.f32();
				if (type == 2) r.skip(8);         // quadratic: forward and backward tangents
				else if (type == 3) r.skip(12);   // TBC
				out.push_back(k);
			}
		}

		void ReadTransformData(const nif::File& f, int block, Track& t)
		{
			nif::Reader r(f.blocks[block].data(), f.blocks[block].size());
			uint32_t nr = r.u32();
			if (nr) {
				uint32_t type = r.u32();
				if (type != 4) {
					for (uint32_t i = 0; i < nr; i++) {
						Track::QKey k;
						k.t = r.f32();
						k.q = ReadQuat(r);
						if (type == 3) r.skip(12);
						t.rotations.push_back(k);
					}
				} else {
					for (auto& axis : t.euler) ReadFloatKeys(r, axis);
				}
			}
			uint32_t nt = r.u32();
			if (nt) {
				uint32_t type = r.u32();
				for (uint32_t i = 0; i < nt; i++) {
					Track::VKey k;
					k.t = r.f32();
					k.v = r.v3();
					if (type == 2) r.skip(24);
					else if (type == 3) r.skip(12);
					t.translations.push_back(k);
				}
			}
			ReadFloatKeys(r, t.scales);
		}

		void ReadSpline(const nif::File& f, nif::Reader& r, Track& t)
		{
			t.spline = true;
			t.start = r.f32();
			t.stop = r.f32();
			int32_t data = r.i32(), basis = r.i32();
			ReadPose(r, t.rest);
			uint32_t handles[3] = { r.u32(), r.u32(), r.u32() };   // translation, rotation, scale
			float bias[3], mult[3];
			for (int i = 0; i < 3; i++) { bias[i] = r.f32(); mult[i] = r.f32(); }
			if (data < 0 || basis < 0) return;
			nif::Reader b(f.blocks[basis].data(), f.blocks[basis].size());
			t.controlPoints = (int)b.u32();
			nif::Reader d(f.blocks[data].data(), f.blocks[data].size());
			uint32_t nf = d.u32();
			d.skip(nf * 4);
			uint32_t nc = d.u32();
			std::vector<int16_t> compact(nc);
			for (auto& c : compact) c = (int16_t)d.u16();
			auto unpack = [&](uint32_t handle, int width, float bias, float mult, std::vector<float>& out) {
				if (handle == 0xFFFF || handle == kNone) return;
				size_t need = handle + (size_t)t.controlPoints * width;
				if (need > compact.size()) return;
				out.resize((size_t)t.controlPoints * width);
				for (size_t i = 0; i < out.size(); i++) out[i] = bias + mult * (compact[handle + i] / 32767.0f);
			};
			unpack(handles[0], 3, bias[0], mult[0], t.splineT);
			unpack(handles[1], 4, bias[1], mult[1], t.splineR);
			unpack(handles[2], 1, bias[2], mult[2], t.splineS);
		}

		// An open uniform cubic B-spline over n control points of `width` floats, at u in [0, 1].
		void EvalSpline(const std::vector<float>& cp, int n, int width, float u, float* out)
		{
			const int p = 3;
			if (n < p + 1) {   // too few for a cubic: hold the first point
				for (int k = 0; k < width; k++) out[k] = cp[k];
				return;
			}
			float param = std::clamp(u, 0.0f, 1.0f) * (n - p);
			auto knot = [&](int i) { return (float)std::clamp(i - p, 0, n - p); };
			int s = std::clamp((int)std::floor(param) + p, p, n - 1);
			float d[4][4];
			for (int j = 0; j <= p; j++)
				for (int k = 0; k < width; k++) d[j][k] = cp[(size_t)(j + s - p) * width + k];
			for (int r = 1; r <= p; r++)
				for (int j = p; j >= r; j--) {
					float lo = knot(j + s - p), hi = knot(j + 1 + s - r);
					float a = hi > lo ? (param - lo) / (hi - lo) : 0.0f;
					for (int k = 0; k < width; k++) d[j][k] = (1 - a) * d[j - 1][k] + a * d[j][k];
				}
			for (int k = 0; k < width; k++) out[k] = d[p][k];
		}

		template <class K>
		size_t Find(const std::vector<K>& keys, float t, float& frac)
		{
			if (t <= keys.front().t) { frac = 0; return 0; }
			if (t >= keys.back().t) { frac = 0; return keys.size() - 1; }
			size_t i = std::upper_bound(keys.begin(), keys.end(), t, [](float v, const K& k) { return v < k.t; }) - keys.begin() - 1;
			float span = keys[i + 1].t - keys[i].t;
			frac = span > 0 ? (t - keys[i].t) / span : 0.0f;
			return i;
		}

		float SampleFloat(const std::vector<Track::FKey>& keys, float t)
		{
			float f;
			size_t i = Find(keys, t, f);
			return i + 1 < keys.size() ? keys[i].v + (keys[i + 1].v - keys[i].v) * f : keys[i].v;
		}

		Quat Normalize(Quat q)
		{
			float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
			if (l < 1e-8f) return Quat();
			return { q.w / l, q.x / l, q.y / l, q.z / l };
		}
	}

	Quat Slerp(Quat a, Quat b, float t)
	{
		float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
		if (d < 0) { b = { -b.w, -b.x, -b.y, -b.z }; d = -d; }
		if (d > 0.9995f)
			return Normalize({ a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t });
		float th = std::acos(d), s = std::sin(th);
		float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
		return { a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb };
	}

	nif::Mat3 ToMatrix(const Quat& q)
	{
		nif::Mat3 m;
		float w = q.w, x = q.x, y = q.y, z = q.z;
		m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - w * z);     m.m[0][2] = 2 * (x * z + w * y);
		m.m[1][0] = 2 * (x * y + w * z);     m.m[1][1] = 1 - 2 * (x * x + z * z); m.m[1][2] = 2 * (y * z - w * x);
		m.m[2][0] = 2 * (x * z - w * y);     m.m[2][1] = 2 * (y * z + w * x);     m.m[2][2] = 1 - 2 * (x * x + y * y);
		return m;
	}

	Quat FromMatrix(const nif::Mat3& m)
	{
		Quat q;
		float tr = m.m[0][0] + m.m[1][1] + m.m[2][2];
		if (tr > 0) {
			float s = std::sqrt(tr + 1.0f) * 2;
			q.w = 0.25f * s; q.x = (m.m[2][1] - m.m[1][2]) / s; q.y = (m.m[0][2] - m.m[2][0]) / s; q.z = (m.m[1][0] - m.m[0][1]) / s;
		} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
			float s = std::sqrt(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2;
			q.w = (m.m[2][1] - m.m[1][2]) / s; q.x = 0.25f * s; q.y = (m.m[0][1] + m.m[1][0]) / s; q.z = (m.m[0][2] + m.m[2][0]) / s;
		} else if (m.m[1][1] > m.m[2][2]) {
			float s = std::sqrt(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2;
			q.w = (m.m[0][2] - m.m[2][0]) / s; q.x = (m.m[0][1] + m.m[1][0]) / s; q.y = 0.25f * s; q.z = (m.m[1][2] + m.m[2][1]) / s;
		} else {
			float s = std::sqrt(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2;
			q.w = (m.m[1][0] - m.m[0][1]) / s; q.x = (m.m[0][2] + m.m[2][0]) / s; q.y = (m.m[1][2] + m.m[2][1]) / s; q.z = 0.25f * s;
		}
		return Normalize(q);
	}

	Clip Load(const std::string& path)
	{
		nif::File f;
		f.load(path);
		int seq = -1;
		for (size_t i = 0; i < f.blocks.size() && seq < 0; i++)
			if (f.typeOf(i) == "NiControllerSequence" || f.typeOf(i) == "BSAnimGroupSequence") seq = (int)i;
		if (seq < 0) throw std::runtime_error("no NiControllerSequence in the file");

		Clip clip;
		nif::Reader r(f.blocks[seq].data(), f.blocks[seq].size());
		clip.name = Str(f, r.u32());
		uint32_t count = r.u32();
		r.u32();   // array grow-by
		struct Block { int32_t interp; std::string node; };
		std::vector<Block> blocks;
		for (uint32_t i = 0; i < count; i++) {
			Block b;
			b.interp = r.i32();
			r.i32();   // controller
			r.u8();    // priority
			b.node = Str(f, r.u32());
			r.u32(); r.u32(); r.u32(); r.u32();   // property type, controller type, controller id, interpolator id
			blocks.push_back(b);
		}
		r.f32();   // weight
		r.i32();   // text keys
		uint32_t cycle = r.u32();
		r.f32();   // frequency
		clip.start = r.f32();
		clip.stop = r.f32();
		r.i32();   // manager
		clip.accumRoot = Str(f, r.u32());
		clip.loops = cycle == 0;   // CYCLE_LOOP

		for (auto& b : blocks) {
			if (b.interp < 0 || b.node.empty()) continue;
			const std::string& type = f.typeOf(b.interp);
			Track t;
			t.node = b.node;
			nif::Reader ir(f.blocks[b.interp].data(), f.blocks[b.interp].size());
			if (type == "NiTransformInterpolator") {
				ReadPose(ir, t.rest);
				int32_t data = ir.i32();
				if (data >= 0) ReadTransformData(f, data, t);
			} else if (type == "NiBSplineCompTransformInterpolator") {
				ReadSpline(f, ir, t);
			} else {
				continue;   // float, bool, point interpolators: morphs, visibility; not the skeleton
			}
			clip.tracks.push_back(std::move(t));
		}
		return clip;
	}

	Pose Sample(const Track& t, float time)
	{
		Pose p = t.rest;
		if (t.spline) {
			float u = t.stop > t.start ? (time - t.start) / (t.stop - t.start) : 0.0f;
			float v[4];
			if (!t.splineT.empty()) { EvalSpline(t.splineT, t.controlPoints, 3, u, v); p.translation = { v[0], v[1], v[2] }; p.hasTranslation = true; }
			if (!t.splineR.empty()) { EvalSpline(t.splineR, t.controlPoints, 4, u, v); p.rotation = Normalize({ v[0], v[1], v[2], v[3] }); p.hasRotation = true; }
			if (!t.splineS.empty()) { EvalSpline(t.splineS, t.controlPoints, 1, u, v); p.scale = v[0]; p.hasScale = true; }
			return p;
		}
		float f;
		if (!t.rotations.empty()) {
			size_t i = Find(t.rotations, time, f);
			p.rotation = i + 1 < t.rotations.size() ? Slerp(t.rotations[i].q, t.rotations[i + 1].q, f) : t.rotations[i].q;
			p.hasRotation = true;
		} else if (!t.euler[0].empty() || !t.euler[1].empty() || !t.euler[2].empty()) {
			float a[3];
			for (int k = 0; k < 3; k++) a[k] = t.euler[k].empty() ? 0.0f : SampleFloat(t.euler[k], time);
			// Rz * Ry * Rx: about X first, then Y, then Z. (Checked with tools/kfcheck: this order puts an
			// idle's pelvis and spine at the skeleton's rest; X * Y * Z turns the pelvis upside down.)
			auto R = [](int axis, float ang) {
				nif::Mat3 m;
				float c = std::cos(ang), s = std::sin(ang);
				int i = (axis + 1) % 3, j = (axis + 2) % 3;
				m.m[i][i] = c; m.m[i][j] = -s; m.m[j][i] = s; m.m[j][j] = c;
				return m;
			};
			p.rotation = FromMatrix(R(2, a[2]) * R(1, a[1]) * R(0, a[0]));
			p.hasRotation = true;
		}
		if (!t.translations.empty()) {
			size_t i = Find(t.translations, time, f);
			auto& a = t.translations[i].v;
			if (i + 1 < t.translations.size()) {
				auto& b = t.translations[i + 1].v;
				p.translation = { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
			} else {
				p.translation = a;
			}
			p.hasTranslation = true;
		}
		if (!t.scales.empty()) { p.scale = SampleFloat(t.scales, time); p.hasScale = true; }
		return p;
	}
}
