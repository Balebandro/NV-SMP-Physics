// XML values: the numbers of an SMP physics file, editable while the previewer runs.
//
// The file's text is kept as it was read; each editable number is a slot (where it sits in that text and its value
// now). The physics is built from the text with the slots' values put in, so nothing else in the file changes (layout,
// comments, names), and saving writes that text back. Editable: the leaf numbers (<mass>0.02</mass>) and the x/y/z
// attributes (<angularLowerLimit x=".." y=".." z=".."/>) inside bone-default, bone, generic-constraint-default,
// per-vertex-shape and per-triangle-shape blocks; weight thresholds are left out (there are dozens).
//
// Chains: each <constraint-group> (a cloth column, a rope) can be made stiffer or softer, calmer or livelier, and given
// more or less side-to-side sway (smp-sway) on its own. Its constraints then get their own values written in, worked out from their template's values now:
//   <generic-constraint bodyA=".." bodyB=".." template="warp" smp-stiff="0.4" smp-lively="-0.2"><angularStiffness .../>
//   <angularLowerLimit .../><angularUpperLimit .../><angularDamping .../></generic-constraint>
// (the plugin reads a constraint's own values over its template's, as FSMP does, and ignores the smp- attributes,
// which keep the chain's slider values for the next time the file is read).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace xmledit
{
	struct Slot
	{
		size_t at = 0, len = 0;   // in the original text
		float value = 0, original = 0;
	};

	// One editable line: a leaf number (1 slot) or an x/y/z vector (3 slots).
	struct Field
	{
		std::string label;        // the element (and its parents inside the block): "mass", "shape/halfExtend"
		std::vector<int> slots;
	};

	struct Block
	{
		std::string label;        // "bone-default cloth", "per-vertex-shape Upperbody:TornCho"
		std::vector<Field> fields;
	};

	// A constraint of a chain: where its element sits in the original text.
	struct Link
	{
		size_t start = 0, end = 0;   // the whole element, "<generic-constraint" .. "/>" or "</generic-constraint>"
		std::string bodyA, bodyB, tmpl;
		// Removed: the constraint is left out (written as a comment) and bodyA made an animated bone (<mass>0</mass> in
		// its <bone> element: it follows its parent as modelled, as the anchors do).
		bool removed = false;
		size_t boneAt = std::string::npos;   // bodyA's <bone> element: where it starts, and where its start tag ends
		size_t boneTagEnd = 0;
		// This constraint's own soft-stiff (-1 .. 2; past 1 its swing narrows, to nothing at 2: locked, it holds its pose
		// on its parent) and calm-lively (-1 .. 1); 0: the template's values. Written as smp-stiff / smp-lively.
		float stiff = 0, lively = 0, origStiff = 0, origLively = 0;
		// Its side-to-side swing (-1 .. 1): the y angular limit (the SMP bones keep the body's axes: x right, y forward,
		// z up, so swinging about y is to the sides). -1: none, 0: the template's, 1: 2.5 times as wide (at most 1.5,
		// near the joint's limit on y). Written as smp-sway.
		float sway = 0, origSway = 0;
	};

	struct Chain
	{
		std::string label;           // the bones' common name: "SMP Cape0 C06"
		std::vector<Link> links;
		float stiff = 0, lively = 0, origStiff = 0, origLively = 0;   // the chain sliders (they set every link's): as its first link's
		float sway = 0, origSway = 0;
	};

	struct Doc
	{
		std::string text;         // the file as read
		std::vector<Slot> slots;
		std::vector<Block> blocks;
		std::vector<Chain> chains;
		int version = 0;          // counts the files parsed (the master sliders start again at the middle for each)

		bool Edited() const
		{
			for (auto& s : slots) if (s.value != s.original) return true;
			for (auto& c : chains) {
				for (auto& l : c.links) if (l.removed || l.stiff != l.origStiff || l.lively != l.origLively || l.sway != l.origSway) return true;
			}
			return false;
		}

		void Reset()
		{
			for (auto& s : slots) s.value = s.original;
			for (auto& c : chains) {
				c.stiff = c.origStiff; c.lively = c.origLively; c.sway = c.origSway;
				for (auto& l : c.links) { l.removed = false; l.stiff = l.origStiff; l.lively = l.origLively; l.sway = l.origSway; }
			}
		}

		// A template's x/y/z field as it is now ("generic-constraint-default warp", "angularStiffness").
		bool TemplateValue(const std::string& tmpl, const char* field, float v[3]) const
		{
			for (auto& b : blocks) {
				if (b.label != "generic-constraint-default " + tmpl) continue;
				for (auto& f : b.fields)
					if (f.label == field && f.slots.size() == 3) { for (int k = 0; k < 3; k++) v[k] = slots[f.slots[k]].value; return true; }
			}
			return false;
		}

		// A chain constraint's element with its own values: its template's, scaled as the master sliders scale them.
		std::string LinkElement(const Link& l) const
		{
			std::string head = "<generic-constraint bodyA=\"" + l.bodyA + "\" bodyB=\"" + l.bodyB + "\" template=\"" + l.tmpl + "\"";
			if (l.stiff == 0 && l.lively == 0 && l.sway == 0) return head + "/>";
			char b[512];
			snprintf(b, sizeof b, " smp-stiff=\"%g\" smp-lively=\"%g\"", l.stiff, l.lively);
			std::string out = head + b;
			if (l.sway != 0) {
				snprintf(b, sizeof b, " smp-sway=\"%g\"", l.sway);
				out += b;
			}
			out += ">";
			const float s = std::min(l.stiff, 1.0f), lock = l.stiff > 1 ? std::max(0.0f, 2.0f - l.stiff) : 1.0f;   // past 1: the swing narrows to none
			float k[3], lo[3], hi[3], d[3];
			if (TemplateValue(l.tmpl, "angularStiffness", k)) {
				for (float& x : k) x = x > 0 ? x * std::pow(10.0f, s) : (s > 0 ? 2.0f * s : 0.0f);
				snprintf(b, sizeof b, "<angularStiffness x=\"%g\" y=\"%g\" z=\"%g\"/>", k[0], k[1], k[2]);
				out += b;
			}
			if (TemplateValue(l.tmpl, "angularLowerLimit", lo) && TemplateValue(l.tmpl, "angularUpperLimit", hi)) {
				for (int i = 0; i < 3; i++)
					for (float* x : { &lo[i], &hi[i] }) {
						float a = i < 2 ? std::min(std::fabs(*x) * std::pow(2.5f, -s), std::max(1.5f, std::fabs(*x))) : std::fabs(*x);   // the twist (z) only narrows when locking
						if (i == 1 && l.sway != 0)   // side to side
							a = l.sway < 0 ? a * (1 + l.sway) : std::min(a * std::pow(2.5f, l.sway), std::max(1.5f, a));
						a *= lock;
						*x = *x < 0 ? -a : a;
					}
				snprintf(b, sizeof b, "<angularLowerLimit x=\"%g\" y=\"%g\" z=\"%g\"/><angularUpperLimit x=\"%g\" y=\"%g\" z=\"%g\"/>", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
				out += b;
			}
			if (TemplateValue(l.tmpl, "angularDamping", d)) {
				for (float& x : d) x = x > 0 ? x * std::pow(10.0f, -l.lively) : (l.lively < 0 ? -0.5f * l.lively : 0.0f);
				snprintf(b, sizeof b, "<angularDamping x=\"%g\" y=\"%g\" z=\"%g\"/>", d[0], d[1], d[2]);
				out += b;
			}
			return out + "</generic-constraint>";
		}

		// The text with the slots' values and the chains' constraints in.
		std::string Compose() const
		{
			struct Edit { size_t at, len; std::string with; };
			std::vector<Edit> edits;
			for (auto& s : slots) {
				if (s.value == s.original) continue;
				char b[32];
				snprintf(b, sizeof b, "%g", s.value);
				edits.push_back({ s.at, s.len, b });
			}
			// A chain is written afresh whenever it or its templates changed (its values follow the templates').
			for (auto& c : chains) {
				bool templates = false;
				for (auto& l : c.links)
					for (auto& bl : blocks)
						if (bl.label == "generic-constraint-default " + l.tmpl)
							for (auto& f : bl.fields) for (int s : f.slots) templates |= slots[s].value != slots[s].original;
				for (auto& l : c.links) {
					bool rewrite = l.stiff != l.origStiff || l.lively != l.origLively || l.sway != l.origSway ||
						(templates && (l.stiff != 0 || l.lively != 0 || l.sway != 0));
					if (l.removed) {
						// The constraint as a comment ("--" cannot appear in one), and its bone animated.
						std::string was = text.substr(l.start, l.end - l.start);
						for (size_t p; (p = was.find("--")) != std::string::npos;) was.replace(p, 2, "- -");
						edits.push_back({ l.start, l.end - l.start, "<!-- removed in the previewer: " + was + " -->" });
						if (l.boneAt != std::string::npos) {
							if (text[l.boneTagEnd - 1] == '/') edits.push_back({ l.boneTagEnd - 1, 2, "><mass>0</mass></bone>" });
							else edits.push_back({ l.boneTagEnd + 1, 0, "<mass>0</mass>" });
						}
					} else if (rewrite)
						edits.push_back({ l.start, l.end - l.start, LinkElement(l) });
				}
			}
			std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at < b.at; });
			std::string out;
			out.reserve(text.size() + 64);
			size_t pos = 0;
			for (auto& e : edits) {
				if (e.at < pos) continue;   // overlapping (cannot happen: slots are not inside chain constraints)
				out.append(text, pos, e.at - pos);
				out += e.with;
				pos = e.at + e.len;
			}
			out.append(text, pos, std::string::npos);
			return out;
		}

		void ParseChains()
		{
			chains.clear();
			auto attr = [&](size_t a, size_t e, const char* name) {
				std::string key = std::string(" ") + name + "=\"";
				size_t p = text.find(key, a);
				if (p == std::string::npos || p > e) return std::string();
				p += key.size();
				size_t q = text.find('"', p);
				return q == std::string::npos || q > e ? std::string() : text.substr(p, q - p);
			};
			// Comments are skipped (a removed constraint is kept in one).
			std::vector<std::pair<size_t, size_t>> comments;
			for (size_t c = 0; (c = text.find("<!--", c)) != std::string::npos;) {
				size_t e = text.find("-->", c);
				e = e == std::string::npos ? text.size() : e + 3;
				comments.push_back({ c, e });
				c = e;
			}
			auto inComment = [&](size_t p) { for (auto& [a, e] : comments) if (p >= a && p < e) return true; return false; };
			// The <bone> elements by name: where each starts and where its start tag ends.
			std::map<std::string, std::pair<size_t, size_t>> bones;
			for (size_t b = 0; (b = text.find("<bone ", b)) != std::string::npos;) {
				size_t e = text.find('>', b);
				if (e == std::string::npos) break;
				std::string n = attr(b, e, "name");
				if (!n.empty() && !inComment(b)) bones.emplace(n, std::make_pair(b, e));
				b = e;
			}
			size_t g = 0;
			while ((g = text.find("<constraint-group>", g)) != std::string::npos) {
				size_t ge = text.find("</constraint-group>", g);
				if (ge == std::string::npos) break;
				Chain c;
				size_t i = g;
				while ((i = text.find("<generic-constraint ", i)) != std::string::npos && i < ge) {
					size_t tagEnd = text.find('>', i);
					if (tagEnd == std::string::npos) break;
					if (inComment(i)) { i = tagEnd; continue; }
					Link l;
					l.start = i;
					l.bodyA = attr(i, tagEnd, "bodyA");
					l.bodyB = attr(i, tagEnd, "bodyB");
					l.tmpl = attr(i, tagEnd, "template");
					if (text[tagEnd - 1] == '/') l.end = tagEnd + 1;
					else {
						size_t close = text.find("</generic-constraint>", tagEnd);
						if (close == std::string::npos || close > ge) break;
						l.end = close + strlen("</generic-constraint>");
					}
					std::string s = attr(i, tagEnd, "smp-stiff"), v = attr(i, tagEnd, "smp-lively"), w = attr(i, tagEnd, "smp-sway");
					if (!s.empty()) l.stiff = l.origStiff = (float)atof(s.c_str());
					if (!v.empty()) l.lively = l.origLively = (float)atof(v.c_str());
					if (!w.empty()) l.sway = l.origSway = (float)atof(w.c_str());
					auto bone = bones.find(l.bodyA);
					if (bone != bones.end()) { l.boneAt = bone->second.first; l.boneTagEnd = bone->second.second; }
					if (!l.tmpl.empty()) c.links.push_back(l);
					i = l.end;
				}
				g = ge + 1;
				if (c.links.empty()) continue;
				c.stiff = c.origStiff = c.links[0].origStiff;
				c.lively = c.origLively = c.links[0].origLively;
				c.sway = c.origSway = c.links[0].origSway;
				// The label: what the bodies' names share, up to a space ("SMP Cape0 C06 R1", "... R2" -> "SMP Cape0 C06").
				std::string p = c.links[0].bodyA;
				for (auto& l : c.links) {
					size_t n = 0;
					while (n < p.size() && n < l.bodyA.size() && p[n] == l.bodyA[n]) n++;
					p.resize(n);
				}
				while (!p.empty() && p.back() != ' ') p.pop_back();
				while (!p.empty() && p.back() == ' ') p.pop_back();
				c.label = p.empty() ? c.links[0].bodyA : p;
				if (c.links[0].tmpl.size()) c.label += "  (" + c.links.back().tmpl + ")";
				chains.push_back(c);
			}
		}

		void Parse(const std::string& t)
		{
			text = t;
			static int parsed = 0;
			version = ++parsed;
			slots.clear();
			blocks.clear();
			static const char* kBlocks[] = { "bone-default", "bone", "generic-constraint-default", "per-vertex-shape", "per-triangle-shape" };
			auto isBlock = [](const std::string& n) { for (auto* b : kBlocks) if (n == b) return true; return false; };
			auto isNumber = [](const std::string& s, size_t a, size_t e) {
				if (a >= e) return false;
				char* end = nullptr;
				std::string sub = s.substr(a, e - a);
				strtod(sub.c_str(), &end);
				return end && *end == 0;
			};
			auto attr = [&](size_t tagStart, size_t tagEnd, const char* name, size_t& at, size_t& len) {
				std::string key = std::string(" ") + name + "=\"";
				size_t p = text.find(key, tagStart);
				if (p == std::string::npos || p > tagEnd) return false;
				at = p + key.size();
				size_t q = text.find('"', at);
				if (q == std::string::npos || q > tagEnd) return false;
				len = q - at;
				return true;
			};

			std::vector<std::string> stack;   // open elements
			int blockDepth = -1;              // the stack depth of the open block, -1: none
			size_t i = 0;
			while ((i = text.find('<', i)) != std::string::npos) {
				if (text.compare(i, 4, "<!--") == 0) { size_t e = text.find("-->", i); i = e == std::string::npos ? text.size() : e + 3; continue; }
				if (text.compare(i, 2, "<?") == 0) { size_t e = text.find("?>", i); i = e == std::string::npos ? text.size() : e + 2; continue; }
				size_t end = text.find('>', i);
				if (end == std::string::npos) break;
				if (text[i + 1] == '/') {   // closing tag
					if (!stack.empty()) stack.pop_back();
					if (blockDepth >= 0 && (int)stack.size() <= blockDepth) blockDepth = -1;
					i = end + 1;
					continue;
				}
				bool selfClosing = text[end - 1] == '/';
				size_t ne = i + 1;
				while (ne < end && !isspace((unsigned char)text[ne]) && text[ne] != '/' && text[ne] != '>') ne++;
				std::string name = text.substr(i + 1, ne - i - 1);

				if (blockDepth < 0 && isBlock(name) && !selfClosing) {
					Block b;
					size_t at, len;
					if (attr(i, end, "name", at, len)) b.label = name + " " + text.substr(at, len);
					else if (attr(i, end, "bodyA", at, len)) {
						b.label = name + " " + text.substr(at, len);
						size_t at2, len2;
						if (attr(i, end, "bodyB", at2, len2)) b.label += " - " + text.substr(at2, len2);
					} else b.label = name;
					blocks.push_back(b);
					blockDepth = (int)stack.size();
					stack.push_back(name);
					i = end + 1;
					continue;
				}

				if (blockDepth >= 0 && name != "weight-threshold") {
					std::string label;
					for (size_t k = blockDepth + 1; k < stack.size(); k++) label += stack[k] + "/";
					label += name;
					// x/y/z attributes
					Field f;
					f.label = label;
					for (const char* a : { "x", "y", "z" }) {
						size_t at, len;
						if (attr(i, end, a, at, len) && isNumber(text, at, at + len)) {
							Slot s; s.at = at; s.len = len; s.value = s.original = (float)atof(text.substr(at, len).c_str());
							f.slots.push_back((int)slots.size());
							slots.push_back(s);
						}
					}
					// a leaf number
					if (!selfClosing) {
						size_t close = text.find('<', end + 1);
						if (close != std::string::npos && text.compare(close, 2 + name.size(), "</" + name) == 0) {
							size_t a = end + 1, e = close;
							while (a < e && isspace((unsigned char)text[a])) a++;
							while (e > a && isspace((unsigned char)text[e - 1])) e--;
							if (isNumber(text, a, e)) {
								Slot s; s.at = a; s.len = e - a; s.value = s.original = (float)atof(text.substr(a, e - a).c_str());
								f.slots.push_back((int)slots.size());
								slots.push_back(s);
							}
						}
					}
					if (!f.slots.empty()) blocks.back().fields.push_back(f);
				}
				if (!selfClosing) stack.push_back(name);
				i = end + 1;
			}
			// Blocks with nothing to edit (bones that only name a template) are dropped.
			std::vector<Block> kept;
			for (auto& b : blocks) if (!b.fields.empty()) kept.push_back(b);
			blocks.swap(kept);
			ParseChains();
		}
	};

	// A slider range for a field, from its name.
	inline void Range(const std::string& label, float value, float& lo, float& hi)
	{
		auto has = [&](const char* s) { return label.find(s) != std::string::npos; };
		if (has("angularLowerLimit") || has("angularUpperLimit")) { lo = -3.1416f; hi = 3.1416f; }
		else if (has("LowerLimit") || has("UpperLimit")) { lo = -10; hi = 10; }
		else if (has("Damping") || has("friction") || has("restitution") || has("gravity-factor")) { lo = 0; hi = 1; }
		else if (has("mass")) { lo = 0; hi = 1; }
		else if (has("Stiffness")) { lo = 0; hi = 20; }
		else if (has("margin") || has("penetration")) { lo = 0; hi = 5; }
		else if (has("wind-factor")) { lo = 0; hi = 3; }
		else if (has("halfExtend")) { lo = 0; hi = 20; }
		else if (has("origin")) { lo = -20; hi = 20; }
		else { lo = 0; hi = 10; }
		if (value < lo) lo = value * 2;
		if (value > hi) hi = value * 2;
	}
}

namespace xmledit
{
	// The master sliders: stiff -1 (soft, relaxed) .. +1 (stiff), heavy -1 (flowy) .. +1 (heavy); 0 is the file.
	// They set, from the file's values, the cloth templates' joints and damping:
	//   stiff: the joint springs back to the modelled shape (angularStiffness, x10 at +1, /10 at -1) and how far the
	//          joints swing (angular limits x and y, /2.5 at +1, x2.5 at -1, never past 1.5 rad: wider lets it fold
	//          into itself). The twist (z) is left alone.
	//   heavy: the bones' damping, 1 - (1 - d) / 10 at +1 (0.95 -> 0.995), 1 - (1 - d) x 10 at -1 (0.95 -> 0.5).
	// Every bone-default and generic-constraint-default but those named anchor, rope, end and link (smpdress's rope
	// templates) is a cloth template. Other slots are left as they are.
	inline bool IsClothTemplate(const std::string& label, const char* kind)
	{
		size_t n = strlen(kind);
		if (label.compare(0, n, kind) != 0 || label.size() <= n || label[n] != ' ') return false;
		std::string name = label.substr(n + 1);
		return name != "anchor" && name != "rope" && name != "end" && name != "link";
	}

	inline void ApplyMasters(Doc& d, float stiff, float heavy)
	{
		for (auto& b : d.blocks) {
			bool bone = IsClothTemplate(b.label, "bone-default"), joint = IsClothTemplate(b.label, "generic-constraint-default");
			if (!bone && !joint) continue;
			for (auto& f : b.fields) {
				if (bone && (f.label == "linearDamping" || f.label == "angularDamping")) {
					for (int s : f.slots) {
						float o = d.slots[s].original;
						float v = 1.0f - (1.0f - o) * std::pow(10.0f, -heavy);
						d.slots[s].value = heavy == 0 ? o : std::clamp(v, 0.0f, 0.995f);
					}
				}
				if (joint && f.label == "angularStiffness") {
					for (int s : f.slots) {
						float o = d.slots[s].original;
						float v = o > 0 ? o * std::pow(10.0f, stiff) : (stiff > 0 ? 2.0f * stiff : 0.0f);
						d.slots[s].value = stiff == 0 ? o : v;
					}
				}
				if (joint && (f.label == "angularLowerLimit" || f.label == "angularUpperLimit") && f.slots.size() == 3) {
					for (int k = 0; k < 2; k++) {   // x and y swing
						auto& sl = d.slots[f.slots[k]];
						float o = sl.original, a = std::fabs(o) * std::pow(2.5f, -stiff);
						a = std::min(a, std::max(1.5f, std::fabs(o)));
						sl.value = stiff == 0 ? o : (o < 0 ? -a : a);
					}
				}
			}
		}
	}
}

namespace xmledit
{
	// The third master slider: calm -1 .. +1 lively; 0 is the file. Sets the cloth joints' damping (angularDamping
	// of the cloth generic-constraint-defaults), x10 at -1 (calm: the joints' swing is braked, jitter dies out),
	// /10 at +1 (lively). A joint without damping in the file gets 0.5 x calm at the calm end.
	inline void ApplyCalm(Doc& d, float lively)
	{
		for (auto& b : d.blocks) {
			if (!IsClothTemplate(b.label, "generic-constraint-default")) continue;
			for (auto& f : b.fields) {
				if (f.label != "angularDamping") continue;
				for (int s : f.slots) {
					float o = d.slots[s].original;
					float v = o > 0 ? o * std::pow(10.0f, -lively) : (lively < 0 ? -0.5f * lively : 0.0f);
					d.slots[s].value = lively == 0 ? o : v;
				}
			}
		}
	}

	// Which bones each constraint template moves (the bodyA of every generic-constraint naming it), for the jitter
	// readout.
	inline std::vector<std::pair<std::string, std::vector<std::string>>> TemplateBodies(const Doc& d)
	{
		std::vector<std::pair<std::string, std::vector<std::string>>> out;
		auto get = [&](size_t a, size_t e, const char* name) {
			std::string key = std::string(" ") + name + "=\"";
			size_t p = d.text.find(key, a);
			if (p == std::string::npos || p > e) return std::string();
			p += key.size();
			size_t q = d.text.find('"', p);
			return q == std::string::npos || q > e ? std::string() : d.text.substr(p, q - p);
		};
		size_t i = 0;
		while ((i = d.text.find("<generic-constraint ", i)) != std::string::npos) {
			size_t e = d.text.find('>', i);
			if (e == std::string::npos) break;
			std::string t = get(i, e, "template"), body = get(i, e, "bodyA");
			if (!t.empty() && !body.empty()) {
				auto it = std::find_if(out.begin(), out.end(), [&](auto& p) { return p.first == t; });
				if (it == out.end()) { out.push_back({ t, {} }); it = out.end() - 1; }
				it->second.push_back(body);
			}
			i = e;
		}
		return out;
	}
}
