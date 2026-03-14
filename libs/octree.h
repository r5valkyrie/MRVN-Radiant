/*
   Loose octree for spatial culling of scene instances.
   Used as a pre-filter during scene traversal to quickly reject
   instances that are outside the view frustum.
 */

#pragma once

#include "math/aabb.h"
#include "math/frustum.h"
#include "cullable.h"
#include "scenelib.h"

#include <vector>
#include <unordered_set>
#include <algorithm>

class SceneOctree
{
	static constexpr int MAX_DEPTH = 5;
	static constexpr int MAX_INSTANCES_PER_LEAF = 32;

	struct Node
	{
		AABB bounds;
		Node* children[8] = {};
		std::vector<scene::Instance*> instances;

		~Node(){
			for ( auto*& c : children ){
				delete c;
				c = nullptr;
			}
		}

		bool isLeaf() const {
			return children[0] == nullptr;
		}
	};

	Node* m_root = nullptr;
	bool m_dirty = true;

	static int octantForPoint( const AABB& nodeBounds, const Vector3& point ){
		int octant = 0;
		if ( point.x() >= nodeBounds.origin.x() ) octant |= 1;
		if ( point.y() >= nodeBounds.origin.y() ) octant |= 2;
		if ( point.z() >= nodeBounds.origin.z() ) octant |= 4;
		return octant;
	}

	static AABB childBounds( const AABB& parent, int octant ){
		Vector3 halfExtents = parent.extents * 0.5f;
		Vector3 childOrigin = parent.origin;
		childOrigin.x() += ( octant & 1 ) ? halfExtents.x() : -halfExtents.x();
		childOrigin.y() += ( octant & 2 ) ? halfExtents.y() : -halfExtents.y();
		childOrigin.z() += ( octant & 4 ) ? halfExtents.z() : -halfExtents.z();
		return AABB( childOrigin, halfExtents );
	}

	static bool aabbContains( const AABB& outer, const AABB& inner ){
		return ( inner.origin.x() - inner.extents.x() >= outer.origin.x() - outer.extents.x() )
		    && ( inner.origin.x() + inner.extents.x() <= outer.origin.x() + outer.extents.x() )
		    && ( inner.origin.y() - inner.extents.y() >= outer.origin.y() - outer.extents.y() )
		    && ( inner.origin.y() + inner.extents.y() <= outer.origin.y() + outer.extents.y() )
		    && ( inner.origin.z() - inner.extents.z() >= outer.origin.z() - outer.extents.z() )
		    && ( inner.origin.z() + inner.extents.z() <= outer.origin.z() + outer.extents.z() );
	}

	static bool aabbIntersects( const AABB& a, const AABB& b ){
		return std::abs( a.origin.x() - b.origin.x() ) <= ( a.extents.x() + b.extents.x() )
		    && std::abs( a.origin.y() - b.origin.y() ) <= ( a.extents.y() + b.extents.y() )
		    && std::abs( a.origin.z() - b.origin.z() ) <= ( a.extents.z() + b.extents.z() );
	}

	void insertInstance( Node* node, scene::Instance* instance, const AABB& instanceBounds, int depth ){
		if ( node->isLeaf() ) {
			node->instances.push_back( instance );
			// Split if too many instances and not at max depth
			if ( depth < MAX_DEPTH && static_cast<int>( node->instances.size() ) > MAX_INSTANCES_PER_LEAF ) {
				splitNode( node, depth );
			}
			return;
		}
		// Internal node: find which children this instance overlaps
		bool inserted = false;
		for ( int i = 0; i < 8; ++i ) {
			if ( node->children[i] && aabbIntersects( node->children[i]->bounds, instanceBounds ) ) {
				insertInstance( node->children[i], instance, instanceBounds, depth + 1 );
				inserted = true;
			}
		}
		if ( !inserted ) {
			// Instance doesn't fit any child (shouldn't happen, but store here as fallback)
			node->instances.push_back( instance );
		}
	}

	void splitNode( Node* node, int depth ){
		// Create 8 children
		for ( int i = 0; i < 8; ++i ) {
			node->children[i] = new Node();
			node->children[i]->bounds = childBounds( node->bounds, i );
		}
		// Redistribute instances
		std::vector<scene::Instance*> old;
		old.swap( node->instances );
		for ( auto* inst : old ) {
			const AABB& ib = inst->worldAABB();
			if ( !aabb_valid( ib ) ) {
				node->instances.push_back( inst );
				continue;
			}
			bool placed = false;
			for ( int i = 0; i < 8; ++i ) {
				if ( aabbIntersects( node->children[i]->bounds, ib ) ) {
					insertInstance( node->children[i], inst, ib, depth + 1 );
					placed = true;
				}
			}
			if ( !placed ) {
				node->instances.push_back( inst );
			}
		}
	}

	void queryNode( const Node* node, const VolumeTest& volume, std::unordered_set<scene::Instance*>& result ) const {
		if ( node == nullptr ) return;

		// Test this node's AABB against the frustum
		VolumeIntersectionValue vis = volume.TestAABB( node->bounds );
		if ( vis == c_volumeOutside ) {
			return; // Entire node is outside frustum
		}

		// Add all instances stored directly in this node
		for ( auto* inst : node->instances ) {
			result.insert( inst );
		}

		if ( vis == c_volumeInside ) {
			// Entire node is inside frustum - add all instances in all children
			collectAll( node, result );
			return;
		}

		// Partially visible - recurse into children
		if ( !node->isLeaf() ) {
			for ( int i = 0; i < 8; ++i ) {
				queryNode( node->children[i], volume, result );
			}
		}
	}

	void collectAll( const Node* node, std::unordered_set<scene::Instance*>& result ) const {
		if ( node == nullptr ) return;
		for ( auto* inst : node->instances ) {
			result.insert( inst );
		}
		if ( !node->isLeaf() ) {
			for ( int i = 0; i < 8; ++i ) {
				collectAll( node->children[i], result );
			}
		}
	}

public:
	SceneOctree() = default;
	~SceneOctree(){
		delete m_root;
	}
	SceneOctree( const SceneOctree& ) = delete;
	SceneOctree& operator=( const SceneOctree& ) = delete;

	void markDirty(){
		m_dirty = true;
	}

	bool isDirty() const {
		return m_dirty;
	}

	void clear(){
		delete m_root;
		m_root = nullptr;
	}

	/// Build the octree from a collection of instances.
	/// Template parameter to accept any container of scene::Instance*.
	template<typename InstanceIterator>
	void build( InstanceIterator begin, InstanceIterator end ){
		clear();

		if ( begin == end ) return;

		// Compute scene bounds
		AABB sceneBounds;
		bool first = true;
		for ( auto it = begin; it != end; ++it ) {
			scene::Instance* inst = ( *it ).second;
			const AABB& ib = inst->worldAABB();
			if ( !aabb_valid( ib ) ) continue;
			if ( first ) {
				sceneBounds = ib;
				first = false;
			}
			else {
				// Extend scene bounds
				aabb_extend_by_aabb( sceneBounds, ib );
			}
		}

		if ( first ) return; // No valid instances

		// Pad the scene bounds slightly
		sceneBounds.extents.x() += 1.0f;
		sceneBounds.extents.y() += 1.0f;
		sceneBounds.extents.z() += 1.0f;

		// Make extents uniform (cube) for balanced octree
		float maxExtent = std::max( { sceneBounds.extents.x(), sceneBounds.extents.y(), sceneBounds.extents.z() } );
		sceneBounds.extents = Vector3( maxExtent, maxExtent, maxExtent );

		m_root = new Node();
		m_root->bounds = sceneBounds;

		// Insert all instances
		for ( auto it = begin; it != end; ++it ) {
			scene::Instance* inst = ( *it ).second;
			const AABB& ib = inst->worldAABB();
			if ( !aabb_valid( ib ) ) {
				m_root->instances.push_back( inst );
				continue;
			}
			insertInstance( m_root, inst, ib, 0 );
		}

		m_dirty = false;
	}

	/// Query the octree for instances potentially visible in the given volume.
	/// Results include all instances in octree nodes that intersect the frustum.
	void queryVisible( const VolumeTest& volume, std::unordered_set<scene::Instance*>& result ) const {
		if ( m_root != nullptr ) {
			queryNode( m_root, volume, result );
		}
	}

	/// Add ancestors of all instances in the set to the set itself.
	/// For each instance, walk up to the root adding all ancestors.
	static void addAncestors( std::unordered_set<scene::Instance*>& visibleSet ){
		std::vector<scene::Instance*> snapshot( visibleSet.begin(), visibleSet.end() );
		for ( auto* inst : snapshot ) {
			scene::Instance* parent = inst->parent();
			while ( parent != nullptr ) {
				if ( visibleSet.count( parent ) ) break; // Already added this chain
				visibleSet.insert( parent );
				parent = parent->parent();
			}
		}
	}
};
