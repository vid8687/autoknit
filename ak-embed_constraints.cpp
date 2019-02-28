#include "pipeline.hpp"
#include "EmbeddedPlanarMap.hpp"


#include <glm/gtx/norm.hpp>
#include <glm/gtx/hash.hpp>

#include <algorithm>
#include <deque>
#include <iostream>
#include <set>
#include <map>
#include <unordered_map>
#include <unordered_set>


void ak::embed_constraints(
	ak::Parameters const &parameters,
	ak::Model const &model,
	std::vector< ak::Constraint > const &constraints,
	ak::Model *constrained_model_,
	std::vector< float > *constrained_values_, //same size as out_model's vertices
	std::vector< std::vector< glm::vec3 > > *DEBUG_chain_paths,
	std::vector< std::vector< glm::vec3 > > *DEBUG_chain_loops
) {
	assert(constrained_model_);
	auto &constrained_model = *constrained_model_;
	constrained_model = ak::Model();

	assert(constrained_values_);
	auto &constrained_values = *constrained_values_;
	constrained_values = std::vector< float >();

	if (DEBUG_chain_paths) {
		*DEBUG_chain_paths = std::vector< std::vector< glm::vec3 > >(constraints.size());
	}

	if (DEBUG_chain_loops) {
		*DEBUG_chain_loops = std::vector< std::vector< glm::vec3 > >(constraints.size());
	}

	//No constraints => return input model:
	if (constraints.empty()) {
		constrained_model = model;
		constrained_values.assign(constrained_model.vertices.size(), std::numeric_limits< float >::quiet_NaN());
		return;
	}

	std::vector< std::vector< std::pair< uint32_t, float > > > adj(model.vertices.size());
	{ //extract edges from model:
		std::set< std::pair< uint32_t, uint32_t > > edges;
		for (auto const &tri : model.triangles) {
			edges.insert(std::minmax(tri.x, tri.y));
			edges.insert(std::minmax(tri.y, tri.z));
			edges.insert(std::minmax(tri.z, tri.x));
		}
		for (auto const &e : edges) {
			float len = glm::length(model.vertices[e.second] - model.vertices[e.first]);
			adj[e.first].emplace_back(e.second, len);
			adj[e.second].emplace_back(e.first, len);
		}
	}

	//find chain paths on original model:
	std::vector< std::vector< uint32_t > > paths;
	for (auto const &cons : constraints) {
		if (cons.chain.empty()) continue;
		std::vector< uint32_t > path;
		for (uint32_t goal : cons.chain) {
			if (path.empty()) {
				path.emplace_back(goal);
				continue;
			}
			std::vector< std::pair< float, uint32_t > > todo;
			std::vector< std::pair< float, uint32_t > > visited(model.vertices.size(), std::make_pair(std::numeric_limits< float >::infinity(), -1U));
			auto visit = [&todo, &visited](uint32_t vertex, float distance, uint32_t from) {
				if (distance < visited[vertex].first) {
					visited[vertex] = std::make_pair(distance, from);
					todo.emplace_back(distance, vertex);
					std::push_heap(todo.begin(), todo.end(), std::greater< std::pair< float, uint32_t > >());
				}
			};
			visit(goal, 0.0f, -1U);
			while (!todo.empty()) {
				std::pop_heap(todo.begin(), todo.end(), std::greater< std::pair< float, uint32_t > >());
				auto at = todo.back();
				todo.pop_back();
				if (at.first > visited[at.second].first) continue;
				if (at.second == path.back()) break;
				for (auto const &a : adj[at.second]) {
					visit(a.first, at.first + a.second, at.second);
				}
			}
			while (path.back() != goal) {
				if (visited[path.back()].second == -1U) {
					std::cerr << "ERROR: constraint chain moves between connected components." << std::endl;
					break;
				}
				path.emplace_back(visited[path.back()].second);
			}
		}
			paths.emplace_back(path);
	}

	//Now create a higher-resolution mesh for trimming / eventually interpolation:

	const float MaxEdgeLength = parameters.get_max_edge_length(); //largest allowed edge length
	constexpr const float MinEdgeRatio = 0.3f; //smallest allowed smallest-to-largest edge ratio in a triangle

	const float MaxEdgeLength2 = MaxEdgeLength * MaxEdgeLength;
	constexpr const float MinEdgeRatio2 = MinEdgeRatio * MinEdgeRatio;

	std::cout << "Max edge length: " << MaxEdgeLength << " model units." << std::endl;

	std::vector< glm::vec3 > verts = model.vertices;
	std::vector< glm::uvec3 > tris = model.triangles;

	//PARANOIA: no degenerate triangles, please
	for (auto const &tri : tris) {
		glm::vec3 const &x = verts[tri.x];
		glm::vec3 const &y = verts[tri.y];
		glm::vec3 const &z = verts[tri.z];
		assert(tri.x != tri.y && tri.x != tri.z && tri.y != tri.z);
		assert(x != y && x != z && y != z);
	}

	/*
	std::vector< EmbeddedVertex > everts;
	everts.reserve(verts.size());
	for (uint32_t i = 0; i < verts.size(); ++i) {
		everts.emplace_back(EmbeddedVertex::on_vertex(i));
	}
	*/

	auto divide = [&verts, &tris, &paths](std::unordered_set< glm::uvec2 > const &marked) {
		assert(!marked.empty());
		std::unordered_map< glm::uvec2, uint32_t > marked_verts;
		marked_verts.reserve(marked.size());

		{ //create new verts in the middle of edges:
			std::vector< glm::ivec2 > edges(marked.begin(), marked.end());
			//sort to avoid any system-specific hash ordering:
			std::sort(edges.begin(), edges.end(), [](glm::uvec2 const &a, glm::uvec2 const &b){
				if (a.x != b.x) return a.x < b.x;
				else return a.y < b.y;
			});
			for (auto const &e : edges) {
				marked_verts.insert(std::make_pair(e, verts.size()));
				verts.emplace_back((verts[e.x] + verts[e.y]) / 2.0f);
			}
		}

		auto lookup = [&marked_verts](uint32_t a, uint32_t b) {
			auto f = marked_verts.find((a < b ? glm::uvec2(a,b) : glm::uvec2(b,a)));
			if (f != marked_verts.end()) return f->second;
			else return -1U;
		};

		//subdivide all paths:
		for (auto &path : paths) {
			std::vector< uint32_t > new_path;
			new_path.emplace_back(path[0]);
			for (uint32_t i = 1; i < path.size(); ++i) {
				uint32_t v = lookup(path[i-1], path[i]);
				if (v != -1U) new_path.emplace_back(v);
				new_path.emplace_back(path[i]);
			}
			path = std::move(new_path);
		}

		//subdivide all tris:
		std::vector< glm::uvec3 > new_tris;

		auto quad = [&new_tris, &verts](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
			float ac = glm::length2(verts[c] - verts[a]);
			float bd = glm::length2(verts[d] - verts[b]);
			if (ac < bd) {
				new_tris.emplace_back(a,b,c);
				new_tris.emplace_back(c,d,a);
			} else {
				new_tris.emplace_back(a,b,d);
				new_tris.emplace_back(b,c,d);
			}
		};
		for (auto const &tri : tris) {
			uint32_t a = tri.x;
			uint32_t b = tri.y;
			uint32_t c = tri.z;
			uint32_t ab = lookup(a,b);
			uint32_t bc = lookup(b,c);
			uint32_t ca = lookup(c,a);

			if (ab != -1U && bc != -1U && ca != -1U) {
				//1 -> 4 subdiv!
				new_tris.emplace_back(a, ab, ca);
				new_tris.emplace_back(b, bc, ab);
				new_tris.emplace_back(c, ca, bc);
				new_tris.emplace_back(ab, bc, ca);
			} else if (ab != -1U && bc != -1U && ca == -1U) {
				//1 -> 3 subdiv!
				//NOTE: should consider recursively subdividing to avoid this case
				quad(a, ab, bc, c);
				new_tris.emplace_back(ab, b, bc);
			} else if (ab != -1U && bc == -1U && ca != -1U) {
				new_tris.emplace_back(a, ab, ca);
				quad(ab, b, c, ca);
			} else if (ab == -1U && bc != -1U && ca != -1U) {
				quad(a, b, bc, ca);
				new_tris.emplace_back(bc, c, ca);
			} else if (ab != -1U && bc == -1U && ca == -1U) {
				//1 -> 2 subdiv!
				new_tris.emplace_back(a, ab, c);
				new_tris.emplace_back(b, c, ab);
			} else if (ab == -1U && bc != -1U && ca == -1U) {
				new_tris.emplace_back(a, b, bc);
				new_tris.emplace_back(bc, c, a);
			} else if (ab == -1U && bc == -1U && ca != -1U) {
				new_tris.emplace_back(a, b, ca);
				new_tris.emplace_back(b, c, ca);
			} else { assert(ab == -1U && bc == -1U && ca == -1U);
				//no subdiv!
				new_tris.emplace_back(a, b, c);
			}
		}
		tris = std::move(new_tris);
	};

	//edge length subdivision:
	while (true) {
		//mark edges for subdivision:
		std::unordered_set< glm::uvec2 > marked;
		auto mark = [&marked](uint32_t a, uint32_t b) {
			if (b < a) std::swap(a,b);
			marked.insert(glm::uvec2(a,b));
		};
		auto is_marked = [&marked](uint32_t a, uint32_t b) {
			if (b < a) std::swap(a,b);
			return marked.find(glm::uvec2(a,b)) != marked.end();
		};
		(void)is_marked;
		(void)MinEdgeRatio2;

		//mark for length:
		for (auto const &tri : tris) {
			float len_ab2 = glm::length2(verts[tri.y] - verts[tri.x]);
			float len_bc2 = glm::length2(verts[tri.z] - verts[tri.y]);
			float len_ca2 = glm::length2(verts[tri.x] - verts[tri.z]);
			if (len_ab2 > MaxEdgeLength2) mark(tri.x, tri.y);
			if (len_bc2 > MaxEdgeLength2) mark(tri.y, tri.z);
			if (len_ca2 > MaxEdgeLength2) mark(tri.z, tri.x);
		}
		/*//avoid 1->3 subdivisions:
		while (true) {
			uint32_t old_size = marked.size();
			for (auto const &tri : tris) {
				uint32_t count =
					  (is_marked(tri.x, tri.y) ? 1 : 0)
					+ (is_marked(tri.y, tri.z) ? 1 : 0)
					+ (is_marked(tri.z, tri.x) ? 1 : 0);
				if (count == 2) {
					mark(tri.x, tri.y);
					mark(tri.y, tri.z);
					mark(tri.z, tri.x);
				}
			}
			if (marked.size() == old_size) break;
		}*/

		//std::cout << "  marked " << marked.size() << " for length." << std::endl;
		/* This seems broken [makes way too many triangles]:
		if (marked.empty()) {
			//mark for ratio:
			while (true) {
				uint32_t old_size = marked.size();
				for (auto const &tri : tris) {
					float len_ab2 = glm::length2(verts[tri.y] - verts[tri.x]);
					float len_bc2 = glm::length2(verts[tri.z] - verts[tri.y]);
					float len_ca2 = glm::length2(verts[tri.x] - verts[tri.z]);
					if (is_marked(tri.x, tri.y)) len_ab2 /= 4.0f;
					if (is_marked(tri.y, tri.z)) len_bc2 /= 4.0f;
					if (is_marked(tri.z, tri.x)) len_ca2 /= 4.0f;

					if (std::min(len_bc2, len_ca2) / len_ab2 < MinEdgeRatio2) mark(tri.x, tri.y);
					if (std::min(len_ab2, len_ca2) / len_bc2 < MinEdgeRatio2) mark(tri.y, tri.z);
					if (std::min(len_ab2, len_bc2) / len_ca2 < MinEdgeRatio2) mark(tri.z, tri.x);
				}
				if (marked.size() == old_size) break;
			}
			std::cout << "  marked " << marked.size() << " for ratio." << std::endl;
		}
		*/

		if (marked.empty()) {
			break;
		}
		divide(marked);
	}
	//std::cout << "After division, have " << tris.size() << " triangles on " << verts.size() << " vertices." << std::endl;

	//PARANOIA: no degenerate triangles, please?
	for (auto const &tri : tris) {
		glm::vec3 const &x = verts[tri.x];
		glm::vec3 const &y = verts[tri.y];
		glm::vec3 const &z = verts[tri.z];
		assert(tri.x != tri.y && tri.x != tri.z && tri.y != tri.z);
		assert(x != y && x != z && y != z);
	}

	if (DEBUG_chain_paths) {
		for (auto const &path : paths) {
			auto &DEBUG_chain_path = (*DEBUG_chain_paths)[&path - &paths[0]];
			for (uint32_t v : path) {
				DEBUG_chain_path.emplace_back(verts[v]);
			}
		}
	}

	adj.assign(verts.size(), std::vector< std::pair< uint32_t, float > >());
	{ //extract edges from subdivided model:
		std::set< std::pair< uint32_t, uint32_t > > edges;
		for (auto const &tri : tris) {
			edges.insert(std::minmax(tri.x, tri.y));
			edges.insert(std::minmax(tri.y, tri.z));
			edges.insert(std::minmax(tri.z, tri.x));
		}
		for (auto const &e : edges) {
			float len = glm::length(verts[e.second] - verts[e.first]);
			adj[e.first].emplace_back(e.second, len);
			adj[e.second].emplace_back(e.first, len);
		}
	}

	std::unordered_map< glm::uvec2, uint32_t > opposite; //vertex opposite each [oriented] triangle edge
	opposite.reserve(tris.size() * 3);
	for (auto const &tri : tris) {
		auto ret_xy = opposite.insert(std::make_pair(glm::uvec2(tri.x, tri.y), tri.z));
		assert(ret_xy.second);
		auto ret_yz = opposite.insert(std::make_pair(glm::uvec2(tri.y, tri.z), tri.x));
		assert(ret_yz.second);
		auto ret_zx = opposite.insert(std::make_pair(glm::uvec2(tri.z, tri.x), tri.y));
		assert(ret_zx.second);
	}

	{ //build (+ add to adj) extra "shortcut" edges by unwrapping triangle neighborhoods:
		std::unordered_map< glm::uvec2, float > min_dis;
		auto get_dis = [&](uint32_t a, uint32_t b) -> float & {
			if (a > b) std::swap(a,b);
			return min_dis.insert(std::make_pair(glm::uvec2(a,b), std::numeric_limits< float >::infinity())).first->second;
		};
		for (auto const &tri : tris) {
			glm::vec2 flat_x, flat_y, flat_z; //original verts
			{
				glm::vec3 const &x = verts[tri.x];
				glm::vec3 const &y = verts[tri.y];
				glm::vec3 const &z = verts[tri.z];
				flat_x = glm::vec2(0.0f, 0.0f);
				flat_y = glm::vec2(glm::length(y-x), 0.0f);

				glm::vec3 xy = glm::normalize(y-x);
				glm::vec3 perp_xy = glm::normalize(glm::cross(glm::cross(y-x, z-x), y-x));
				float along = glm::dot(z-x, xy);
				float perp = glm::dot(z-x, perp_xy);

				flat_z = glm::vec2(along, perp);

				//std::cout << "x: (" << x.x << ", " << x.y << ", " << x.z << ") -> (" << flat_x.x << ", " << flat_x.y << ")" << std::endl; //DEBUG
				//std::cout << "y: (" << y.x << ", " << y.y << ", " << y.z << ") -> (" << flat_y.x << ", " << flat_y.y << ")" << std::endl; //DEBUG
				//std::cout << "z: (" << z.x << ", " << z.y << ", " << z.z << ") -> (" << flat_z.x << ", " << flat_z.y << ")" << std::endl; //DEBUG

			}

			//look through edge [ai,bi] from point [root], where edge [ai,bi] is ccw oriented.

			auto is_ccw = [](glm::vec2 const &a, glm::vec2 const &b, glm::vec2 const &c) {
				return glm::dot(glm::vec2(-(b.y-a.y),(b.x-a.x)), c-a) > 0.0f;
			};

			std::function< void(uint32_t, uint32_t, glm::vec2 const &, uint32_t, glm::vec2 const &, uint32_t, glm::vec2 const &, glm::vec2 const &, glm::vec2 const &) > unfold = [&](uint32_t depth, uint32_t root, glm::vec2 const &flat_root, uint32_t ai, glm::vec2 const &flat_a, uint32_t bi, glm::vec2 const &flat_b, glm::vec2 const &limit_a, glm::vec2 const &limit_b) {
				//std::cout << "r: " << root << ": (" << flat_root.x << ", " << flat_root.y << ")" << std::endl; //DEBUG
				//std::cout << "a: " << ai << ": (" << flat_a.x << ", " << flat_a.y << ")" << std::endl; //DEBUG
				//std::cout << "b: " << bi << ": (" << flat_b.x << ", " << flat_b.y << ")" << std::endl; //DEBUG
				assert(is_ccw(flat_root, flat_a, flat_b));
				//should go 'a - limit_a - limit_b - b':
				//assert(flat_a == limit_a || is_ccw(flat_root, flat_a, limit_a));
				assert(is_ccw(flat_root, limit_a, limit_b));
				//assert(flat_b == limit_b || is_ccw(flat_root, limit_b, flat_b));

				uint32_t ci;
				glm::vec2 flat_c;
				{ //if there is a triangle over the ai->bi edge, find other vertex and flatten it:
					auto f = opposite.find(glm::uvec2(bi, ai));
					if (f == opposite.end()) return;
					ci = f->second;
					//figure out c's position along ab and distance from ab:
					glm::vec3 const &a = verts[ai];
					glm::vec3 const &b = verts[bi];
					glm::vec3 const &c = verts[ci];

					glm::vec3 ab = glm::normalize(b-a);
					float along = glm::dot(c-a, ab);
					float perp = -glm::length(c-a - ab*along);

					glm::vec2 flat_ab = glm::normalize(flat_b - flat_a);
					glm::vec2 flat_perp_ab = glm::vec2(-flat_ab.y, flat_ab.x);

					flat_c = flat_a + flat_ab * along + flat_perp_ab * perp;
				}

				//std::cout << "c: " << ci << ": (" << flat_c.x << ", " << flat_c.y << ")" << std::endl; //DEBUG

				//flat_a and flat_b should always be outside limit, it seems like we need to test anyway (thanks, numerics)

				bool ccw_rac = is_ccw(flat_root, limit_a, flat_c) && is_ccw(flat_root, flat_a, flat_c);
				bool ccw_rcb = is_ccw(flat_root, flat_c, limit_b) && is_ccw(flat_root, flat_c, flat_b);

				if (ccw_rac && ccw_rcb) {
					float &dis = get_dis(root, ci);
					dis = std::min(dis, glm::length(flat_root - flat_c));

					//PARANOIA:
					float dis3 = glm::length(verts[root] - verts[ci]);
					if (dis3 > dis + 1e-6) {
						std::cerr << "dis3: " << dis3 << " vs flat dis " << dis << " seems bad!" << std::endl;
						std::cerr << "  ra3: " << glm::length(verts[root] - verts[ai]) << " vs ra: " << glm::length(flat_root - flat_a) << std::endl;
						std::cerr << "  rb3: " << glm::length(verts[root] - verts[bi]) << " vs rb: " << glm::length(flat_root - flat_b) << std::endl;
						std::cerr << "  ab3: " << glm::length(verts[ai] - verts[bi]) << " vs ab: " << glm::length(flat_a - flat_b) << std::endl;
						std::cerr << "  ac3: " << glm::length(verts[ai] - verts[ci]) << " vs ac: " << glm::length(flat_a - flat_c) << std::endl;
						std::cerr << "  bc3: " << glm::length(verts[bi] - verts[ci]) << " vs bc: " << glm::length(flat_b - flat_c) << std::endl;
						assert(dis3 < dis + 1e-6);
					}

					if (depth > 1) {
						assert(is_ccw(flat_root, flat_a, flat_c));
						unfold(depth - 1, root, flat_root, ai, flat_a, ci, flat_c, limit_a, flat_c);
						assert(is_ccw(flat_root, flat_c, flat_b));
						unfold(depth - 1, root, flat_root, ci, flat_c, bi, flat_b, flat_c, limit_b);
					}
				} else if (ccw_rac && !ccw_rcb) {
					if (depth > 1) {
						//assert(!is_ccw(flat_root, flat_c, limit_b)); //DEBUG
						//assert(is_ccw(flat_root, limit_b, flat_c)); //DEBUG -- fails sometimes [thanks, numerics]
						assert(is_ccw(flat_root, flat_a, flat_c));
						unfold(depth - 1, root, flat_root, ai, flat_a, ci, flat_c, limit_a, limit_b);
					}
				} else if (!ccw_rac && ccw_rcb) {
					if (depth > 1) {
						assert(is_ccw(flat_root, flat_c, flat_b));
						unfold(depth - 1, root, flat_root, ci, flat_c, bi, flat_b, limit_a, limit_b);
					}
				}
			};

			const constexpr uint32_t D = 3; //depth to unfold triangles to for more adjacency information; makes slightly nicer geodesics at the expense of increased compute time.

			if (D > 0) {
				unfold(D, tri.x, flat_x, tri.y, flat_y, tri.z, flat_z, flat_y, flat_z);
				unfold(D, tri.y, flat_y, tri.z, flat_z, tri.x, flat_x, flat_z, flat_x);
				unfold(D, tri.z, flat_z, tri.x, flat_x, tri.y, flat_y, flat_x, flat_y);
			}
		}
		for (uint32_t x = 0; x < verts.size(); ++x) {
			for (auto const &yd : adj[x]) {
				float &dis = get_dis(x, yd.first);
				dis = std::min(dis, yd.second);
			}
		}

		//clear adj + re-create from min_dis:
		uint32_t old_adj = 0;
		for (auto const &a : adj) {
			old_adj += a.size();
		}

		adj.assign(verts.size(), std::vector< std::pair< uint32_t, float > >());

		for (auto const &xyd : min_dis) {
			assert(xyd.first.x != xyd.first.y);
			adj[xyd.first.x].emplace_back(xyd.first.y, xyd.second);
			adj[xyd.first.y].emplace_back(xyd.first.x, xyd.second);
		}

		uint32_t new_adj = 0;
		for (auto const &a : adj) {
			new_adj += a.size();
		}

		//std::cout << "Went from " << old_adj << " to " << new_adj << " by unfolding triangles." << std::endl;

		//for consistency:
		for (auto &a : adj) {
			std::sort(a.begin(), a.end());
		}
	}

	//uint32_t used_edges = 0;

	std::vector< std::vector< EmbeddedVertex > > embedded_chains;

	for (auto const &cons : constraints) {
		embedded_chains.emplace_back();

		auto const &path = paths[&cons - &constraints[0]];
		if (cons.radius == 0.0f) {
			//add directly to embedded constrained edges.
			for (auto v : path) {
				assert(v < verts.size());
				embedded_chains.back().emplace_back(EmbeddedVertex::on_vertex(v));
			}
			continue;
		}
		//generate distance field from constraint:
		std::vector< std::pair< float, uint32_t > > todo;
		std::vector< float > distances(verts.size(), std::numeric_limits< float >::infinity());
		auto visit = [&todo, &distances](uint32_t vertex, float distance) {
			if (distance < distances[vertex]) {
				distances[vertex] = distance;
				todo.emplace_back(distance, vertex);
				std::push_heap(todo.begin(), todo.end(), std::greater< std::pair< float, uint32_t > >());
			}
		};
		for (uint32_t i = 0; i < path.size(); ++i) {
			visit(path[i], -cons.radius);
		}
		/*auto do_edge = [&](uint32_t ai, uint32_t bi) {
			auto f = opposite.find(glm::uvec2(ai, bi));
			if (f == opposite.end()) return;
			uint32_t ci = f->second;
			glm::vec3 const &a = verts[ai];
			glm::vec3 const &b = verts[bi];
			glm::vec3 const &c = verts[ci];
			float along = glm::dot(c - a, b - a);
			if (along <= 0.0f) return;
			float lim = glm::dot(b - a, b - a);
			if (along >= lim) return;
			//++used_edges;
			glm::vec3 close = glm::mix(a, b, along / lim);
			visit(ci, glm::length(c - close) - cons.radius);
		};
		for (uint32_t i = 1; i < path.size(); ++i) {
			do_edge(path[i-1], path[i]);
			do_edge(path[i], path[i-1]);
		}*/

		while (!todo.empty()) {
			std::pop_heap(todo.begin(), todo.end(), std::greater< std::pair< float, uint32_t > >());
			auto at = todo.back();
			todo.pop_back();
			if (at.first > distances[at.second]) continue;
			if (at.first > 0.0f) break; //once we start expanding things that are past the contour, no need to continue (TODO: consider blur radius)
			for (auto const &a : adj[at.second]) {
				visit(a.first, at.first + a.second);
			}
		}

		//read back embedded path.

		std::unordered_map< glm::uvec2, EmbeddedVertex > embedded_pts;
		std::unordered_map< glm::uvec2, glm::vec3 > pts;
		auto add = [&distances,&verts,&pts,&embedded_pts](uint32_t a, uint32_t b) {
			assert(distances[a] < 0.0f && distances[b] >= 0.0f);
			float mix = (0.0f - distances[a]) / (distances[b] - distances[a]);
			pts[glm::uvec2(a,b)] = glm::mix(verts[a], verts[b], mix);
			embedded_pts[glm::uvec2(a,b)] = EmbeddedVertex::on_edge(a,b,mix);
			return glm::uvec2(a,b);
		};
		std::unordered_map< glm::uvec2, glm::uvec2 > links;
		std::unordered_map< glm::uvec2, glm::uvec2 > back_links;
		auto link = [&links,&back_links](glm::uvec2 f, glm::uvec2 t) {
			auto res = links.insert(std::make_pair(f, t));
			assert(res.second);
			auto res2 = back_links.insert(std::make_pair(t, f));
			assert(res2.second);
		};
		for (auto const &tri : tris) {
			uint32_t a = tri.x;
			uint32_t b = tri.y;
			uint32_t c = tri.z;
			//spin triangle until 'a' is the minimum distance value:
			for (uint32_t i = 0; i < 3; ++i) {
				if (distances[a] <= distances[b] && distances[a] <= distances[c]) break;
				uint32_t t = a; a = b; b = c; c = t;
			}
			//NOTE: we treat 0.0f as "0.0f + epsilon"
			if (distances[a] >= 0.0f) continue; //all above border
			assert(distances[a] < 0.0f);

			if (distances[b] >= 0.0f && distances[c] >= 0.0f) {
				//edge is from ab to ca
				link(add(a,b), add(a,c));
			} else if (distances[b] >= 0.0f && distances[c] < 0.0f) {
				//edge is from ab to bc
				link(add(a,b), add(c,b));
			} else if (distances[b] < 0.0f && distances[c] >= 0.0f) {
				//edge is from bc to ca
				link(add(b,c), add(a,c));
			} else {
				assert(distances[b] < 0.0f && distances[c] < 0.0f);
				//all below border, nothing to do.
			}
		}

		//read back path from links:
		if (!links.empty()) {
			std::deque< glm::uvec2 > loop;
			loop.emplace_back(links.begin()->first);
			while (true) {
				auto f = links.find(loop.back());
				if (f == links.end()) break;
				loop.emplace_back(f->second);
				if (f->second == loop[0]) break;
			}
			if (loop[0] != loop.back()) {
				while (true) {
					auto f = back_links.find(loop[0]);
					if (f == back_links.end()) break;
					if (f->second == loop.back()) break;
					loop.emplace_front(f->second);
				}
			}

			for (glm::uvec2 e : loop) {
				auto f = embedded_pts.find(e);
				assert(f != embedded_pts.end());
				embedded_chains.back().emplace_back(f->second);
			}

			if (DEBUG_chain_loops) {
				auto &DEBUG_chain_loop = (*DEBUG_chain_loops)[&cons - &constraints[0]];
				for (glm::uvec2 e : loop) {
					auto f = pts.find(e);
					assert(f != pts.end());
					DEBUG_chain_loop.emplace_back(f->second);
				}
			}

		}
	}

	//should have a chain per constraint:
	assert(embedded_chains.size() == constraints.size());

	//embed chains using planar map:
	EmbeddedPlanarMap< float, SameValue< float >, ReplaceValue< float > > epm;
	uint32_t total_chain_edges = 0;
	for (uint32_t c = 0; c < constraints.size(); ++c) {
		uint32_t first = 0;
		uint32_t last = 0;
		for (uint32_t i = 0; i + 1 < embedded_chains[c].size(); ++i) {
			uint32_t a = epm.add_vertex(embedded_chains[c][i]);
			uint32_t b = epm.add_vertex(embedded_chains[c][i+1]);
			epm.add_edge(a,b,constraints[c].value);
			++total_chain_edges;
			if (i == 0) first = a;
			if (i + 2 == embedded_chains[c].size()) last = b;
		}
		//if (first != last) std::cout << "NOTE: have open chain." << std::endl;
	}
	uint32_t total_simplex_edges = 0;
	for (const auto &edges : epm.simplex_edges) {
		total_simplex_edges += edges.second.size();
	}
	/*//DEBUG:
	std::cout << "EPM has " << epm.vertices.size() << " vertices." << std::endl;
	std::cout << "EPM has " << epm.simplex_vertices.size() << " simplices with vertices." << std::endl;
	std::cout << "EPM has " << epm.simplex_edges.size() << " simplices with edges (" << total_simplex_edges << " edges from " << total_chain_edges << " chain edges)." << std::endl;

	for (const auto &se : epm.simplex_edges) {
		assert(se.first.x <= se.first.y && se.first.y <= se.first.z);
		if (se.first.z != -1U) {
			std::cout << se.first.x << ", " << se.first.y << ", " << se.first.z << std::endl;
		}
	}*/

	{ //Build a mesh that is split at the embedded edges:
		std::vector< ak::EmbeddedVertex > split_evs;
		std::vector< glm::uvec3 > split_tris;
		std::vector< uint32_t > epm_to_split;

		epm.split_triangles(verts, tris, &split_evs, &split_tris, &epm_to_split);

		std::vector< glm::vec3 > split_verts;
		split_verts.reserve(split_evs.size());
		for (auto const &ev : split_evs) {
			split_verts.emplace_back(ev.interpolate(verts));
		}

		//record constrained edges in terms of split_verts:
		std::unordered_map< glm::uvec2, float > constrained_edges;
		std::vector< float > split_values(split_verts.size(), std::numeric_limits< float >::quiet_NaN());
		for (const auto &se : epm.simplex_edges) {
			for (auto const &e : se.second) {
				glm::uvec2 ab = glm::uvec2(epm_to_split[e.first], epm_to_split[e.second]);
				if (ab.x > ab.y) std::swap(ab.x, ab.y);
				constrained_edges.insert(std::make_pair(ab, e.value));
				//also grab vertex values:
				split_values[epm_to_split[e.first]] = e.value;
				split_values[epm_to_split[e.second]] = e.value;
			}
		}
		std::cout << constrained_edges.size() << " constrained edges." << std::endl;

		
		std::vector< uint32_t > tri_component(split_tris.size(), -1U);
		std::vector< bool > component_keep;
		{ //mark connected components + delete the "wrong" ones
			std::unordered_map< glm::uvec2, uint32_t > over;
			for (const auto &tri : split_tris) {
				uint32_t ti = &tri - &split_tris[0];
				auto res = over.insert(std::make_pair(glm::uvec2(tri.x, tri.y), ti));
				assert(res.second);
				res = over.insert(std::make_pair(glm::uvec2(tri.y, tri.z), ti));
				assert(res.second);
				res = over.insert(std::make_pair(glm::uvec2(tri.z, tri.x), ti));
				assert(res.second);
			}
			for (uint32_t seed = 0; seed < split_tris.size(); ++seed) {
				if (tri_component[seed] != -1U) continue;
				//std::cout << "Doing CC with seed " << seed << std::endl; //DEBUG
				uint32_t component = component_keep.size();
				tri_component[seed] = component;
				std::set< float > values;
				std::vector< uint32_t > todo;
				todo.emplace_back(seed);
				auto do_edge = [&](uint32_t a, uint32_t b) {
					{ //if edge is constrained, don't traverse over:
						glm::uvec2 e(a,b);
						if (e.x > e.y) std::swap(e.x,e.y);
						auto v = constrained_edges.find(e);
						if (v != constrained_edges.end()) {
							values.insert(v->second);
							return;
						}
					}
					//otherwise, traverse over:
					auto f = over.find(glm::uvec2(b,a));
					if (f != over.end()) {
						if (tri_component[f->second] != component) {
							assert(tri_component[f->second] == -1U);
							tri_component[f->second] = component;
							todo.emplace_back(f->second);
						}
					}
				};
				while (!todo.empty()) {
					uint32_t at = todo.back();
					todo.pop_back();
					assert(tri_component[at] == component);
					do_edge(split_tris[at].x, split_tris[at].y);
					do_edge(split_tris[at].y, split_tris[at].z);
					do_edge(split_tris[at].z, split_tris[at].x);
				}
				component_keep.emplace_back(values.size() > 1);
			}
			std::cout << "Have " << component_keep.size() << " connected components." << std::endl;
		}


		//remove any split_verts that aren't used:
		std::vector< glm::vec3 > compressed_verts;
		std::vector< float > compressed_values;
		std::vector< glm::uvec3 > compressed_tris;
		for (uint32_t ti = 0; ti < split_tris.size(); ++ti) {
			if (component_keep[tri_component[ti]]) {
				compressed_tris.emplace_back(split_tris[ti]);
			}
		}
		compressed_verts.reserve(split_verts.size());
		std::vector< uint32_t > to_compressed(split_verts.size(), -1U);
		auto add_vert = [&](uint32_t vi) {
			if (to_compressed[vi] == -1U) {
				to_compressed[vi] = compressed_verts.size();
				compressed_verts.emplace_back(split_verts[vi]);
				compressed_values.emplace_back(split_values[vi]);
			}
			return to_compressed[vi];
		};
		for (auto &tri : compressed_tris) {
			tri.x = add_vert(tri.x);
			tri.y = add_vert(tri.y);
			tri.z = add_vert(tri.z);
		}

		std::cout << "Went from " << tris.size() << " to (via split) " << split_tris.size() << " to (via discard) " << compressed_tris.size() << " triangles." << std::endl; //DEBUG

		constrained_model.vertices = compressed_verts;
		constrained_model.triangles = compressed_tris;

		constrained_values = compressed_values;

	}


	//std::cout << "Used " << used_edges << " edges." << std::endl; //DEBUG

	//TODO: split at distance field level set
	//TODO: constrain values at distance field border

/*
	constrained_model.vertices = verts;
	constrained_model.triangles = tris;
	*/

/*

	constrained_values.assign(constrained_model.vertices.size(), std::numeric_limits< float >::quiet_NaN());
	//Quick hack to test interpolation:
	uint32_t lowest = 0;
	uint32_t highest = 0;
	for (uint32_t i = 0; i < constrained_model.vertices.size(); ++i) {
		if (constrained_model.vertices[i].z < constrained_model.vertices[lowest].z) lowest = i;
		if (constrained_model.vertices[i].z > constrained_model.vertices[highest].z) highest = i;
	}
	if (!constrained_values.empty()) {
		constrained_values[lowest] =-1.0f;
		constrained_values[highest] = 1.0f;
	}
*/

}
