#ifndef KOKKOS_MERKLE_TREE_HPP
#define KOKKOS_MERKLE_TREE_HPP
#include <Kokkos_Core.hpp>
#include <Kokkos_UnorderedMap.hpp>
#include <climits>
#include "hash_functions.hpp"
#include "map_helpers.hpp"
#include "kokkos_queue.hpp"

//template<uint32_t N>
class MerkleTree {
public:
  Kokkos::View<HashDigest*> tree_d;
  Kokkos::View<HashDigest*>::HostMirror tree_h;

  MerkleTree(const uint32_t num_leaves) {
    tree_d = Kokkos::View<HashDigest*>("Merkle tree", (2*num_leaves-1));
    tree_h = Kokkos::create_mirror_view(tree_d);
  }
  
  KOKKOS_INLINE_FUNCTION HashDigest& operator()(int32_t i) const {
    return tree_d(i);
  }
 
  void digest_to_hex_(const uint8_t digest[20], char* output) {
    int i,j;
    char* c = output;
    for(i=0; i<20/4; i++) {
      for(j=0; j<4; j++) {
        sprintf(c, "%02X", digest[i*4 + j]);
        c += 2;
      }
      sprintf(c, " ");
      c += 1;
    }
    *(c-1) = '\0';
  }

  void print() {
    Kokkos::deep_copy(tree_h, tree_d);
    uint32_t num_leaves = (tree_h.extent(0)+1)/2;
    printf("============================================================\n");
    char buffer[80];
    unsigned int counter = 2;
    for(unsigned int i=0; i<2*num_leaves-1; i++) {
      digest_to_hex_((uint8_t*)(tree_h(i).digest), buffer);
      printf("Node: %u: %s \n", i, buffer);
      if(i == counter) {
        printf("\n");
        counter += 2*counter;
      }
    }
    printf("============================================================\n");
  }
};

template<typename Scheduler, class Hasher>
struct CreateTreeTask {
  using sched_type  = Scheduler;
  using future_type = Kokkos::BasicFuture<uint32_t, Scheduler>;
  using value_type  = uint32_t;

  uint32_t node;
  MerkleTree tree;
  Kokkos::View<uint8_t*> data;
  uint32_t chunk_size;
  Hasher hasher;
  future_type child_l_fut;
  future_type child_r_fut;

  KOKKOS_INLINE_FUNCTION
  CreateTreeTask(const uint32_t n, Hasher& _hasher, const MerkleTree& merkle_tree, const Kokkos::View<uint8_t*>& _data, const uint32_t size) : 
                  node(n), hasher(_hasher), tree(merkle_tree), data(_data), chunk_size(size) {}

  KOKKOS_INLINE_FUNCTION
  void operator()(typename sched_type::member_type& member, uint32_t& result) {
    auto& sched = member.scheduler();

    uint32_t num_chunks = data.size()/chunk_size;
    if(num_chunks*chunk_size < data.size())
      num_chunks += 1;
    const uint32_t leaf_start = num_chunks-1;

    if((node >= leaf_start) || !child_l_fut.is_null() && !child_r_fut.is_null()) {
      uint32_t num_bytes = chunk_size;
      if((node-leaf_start) == num_chunks-1)
        num_bytes = data.size()-((node-leaf_start)*chunk_size);
      if(node >= leaf_start) {
        hasher.hash(data.data()+((node-leaf_start)*chunk_size), 
                    num_bytes, 
                    (uint8_t*)(tree(node).digest));
      } else {
        hasher.hash((uint8_t*)&tree(2*node+1), 2*hasher.digest_size(), (uint8_t*)&tree(node));
      }
      result = 1;
    } else {
      int active_children = 0;
      uint32_t child_l = 2*node+1;
      if(child_l < tree.tree_d.extent(0)) {
        child_l_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CreateTreeTask(child_l, hasher, tree, data, chunk_size));
        active_children += 1;
      }
      uint32_t child_r = 2*node+2;
      if(child_r < tree.tree_d.extent(0)) {
        child_r_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CreateTreeTask(child_r, hasher, tree, data, chunk_size));
        active_children += 1;
      }
      if(active_children == 2) {
        Kokkos::BasicFuture<void, Scheduler> dep[] = {child_l_fut, child_r_fut};
        Kokkos::BasicFuture<void, Scheduler> all_children = sched.when_all(dep, 2);
        Kokkos::respawn(this, all_children, Kokkos::TaskPriority::High);
      }
    }
  }
};

size_t estimate_required_memory(int n_nodes) {
  return n_nodes*2000;
}


template <class Hasher>
void create_merkle_tree_task(Hasher& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size) {
  using scheduler_type = Kokkos::TaskScheduler<Kokkos::DefaultExecutionSpace>;
  using memory_space = typename scheduler_type::memory_space;
  using memory_pool = typename scheduler_type::memory_pool;
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  const uint32_t num_nodes = 2*num_chunks-1;
  auto mpool = memory_pool(memory_space{}, estimate_required_memory(2*num_nodes-1));
  auto root_sched = scheduler_type(mpool);
  Kokkos::BasicFuture<uint32_t, scheduler_type> f = Kokkos::host_spawn(Kokkos::TaskSingle(root_sched), 
                                                                  CreateTreeTask<scheduler_type, Hasher>(0, hasher, tree, data, chunk_size));
  Kokkos::wait(root_sched);
}

template <class Hasher>
void create_merkle_tree(Hasher& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size, const int32_t n_levels=INT_MAX) {
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  const uint32_t num_nodes = 2*num_chunks-1;
  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
  int32_t stop_level = 0;
  if(n_levels < num_levels)
    stop_level = num_levels-n_levels;
  const uint32_t leaf_start = num_chunks-1;
  for(int32_t level=num_levels-1; level>=stop_level; level--) {
    uint32_t nhashes = 1 << level;
    uint32_t start_offset = nhashes-1;
    if(start_offset + nhashes > num_nodes)
      nhashes = num_nodes - start_offset;
    auto range_policy = Kokkos::RangePolicy<>(start_offset, start_offset+nhashes);
    Kokkos::parallel_for("Build tree", range_policy, KOKKOS_LAMBDA(const int i) {
      uint32_t num_bytes = chunk_size;
      if((i-leaf_start) == num_chunks-1)
        num_bytes = data.size()-((i-leaf_start)*chunk_size);
      if(i >= leaf_start) {
        hasher.hash(data.data()+((i-leaf_start)*chunk_size), 
                    num_bytes, 
                    (uint8_t*)(tree(i).digest));
      } else {
        hasher.hash((uint8_t*)&tree(2*i+1), 2*hasher.digest_size(), (uint8_t*)&tree(i));
      }
    });
  }
  Kokkos::fence();
}

template <class Hasher>
MerkleTree create_merkle_tree(Hasher& hasher, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size) {
  uint32_t num_chunks = data.size()/chunk_size;
  if(num_chunks*chunk_size < data.size())
    num_chunks += 1;
  MerkleTree tree = MerkleTree(num_chunks);
  create_merkle_tree(hasher, tree, data, chunk_size, INT_MAX);
  return tree;
}

//template <class Hasher>
//void create_merkle_tree_subtrees(Hahser& hasher, MerkleTree& tree, Kokkos::View<uint8_t*>& data, const uint32_t chunk_size) {
//  uint32_t num_chunks = data.size()/chunk_size;
//  if(num_chunks*chunk_size < data.size())
//    num_chunks += 1;
//  const uint32_t num_nodes = 2*num_chunks-1;
//  const uint32_t num_levels = static_cast<uint32_t>(ceil(log2(num_nodes+1)));
//  constexpr uint32_t num_threads = 128;
//  const uint32_t leaf_start = num_chunks-1;
//  uint32_t num_leagues = num_chunks/num_threads;
//  if(num_threads*num_leagues < num_chunks)
//    num_leagues += 1;
//  Kokkos::TeamPolicy<> team_policy(num_leagues, Kokkos::AUTO());
//  using team_member_type = Kokkos::TeamPolicy<>::member_type;
//  Kokkos::parallel_for("Build tree by subtrees", team_policy, KOKKOS_LAMBDA(team_member_type team_member) {
//    uint32_t league_offset = team_member.league_rank()*num_threads;
//    uint32_t active_threads = 128;
//    uint32_t n_level = num_levels-1;
//    while(active_threads > 0) {
//      Kokkos::parallel_for("Compute level of subtree", Kokkos::RangePolicy<>(0, active_threads), KOKKOS_LAMBDA(const uint32_t j) {
//        uint32_t i = league_offset + j;
//        uint32_t num_bytes = chunk_size;
//        if((i-leaf_start) == num_chunks-1)
//          num_bytes = data.size()-((i-leaf_start)*chunk_size);
//        if(i >= leaf_start) {
//          hasher.hash(data.data()+((i-leaf_start)*chunk_size), 
//                      num_bytes, 
//                      (uint8_t*)(tree(i).digest));
//        } else {
//          hasher.hash((uint8_t*)&tree(2*i+1), 2*hasher.digest_size(), (uint8_t*)&tree(i));
//        }
//      });
//    }
//  });
//}

void compare_trees_fused(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, DistinctMap& distinct_map) {
  uint32_t num_comp = 0;
  uint32_t q_size = queue.size();
  while(q_size > 0) {
    num_comp += q_size;
    Kokkos::parallel_for("Compare trees", Kokkos::RangePolicy<>(0, q_size), KOKKOS_LAMBDA(const uint32_t entry) {
      uint32_t node = queue.pop();
      HashDigest digest = tree(node);
      NodeInfo info(node, node, tree_id);
      auto result = distinct_map.insert(digest, info); // Try to insert
      if(result.success()) { // Node is distinct
        uint32_t child_l = 2*node+1;
        if(child_l < queue.capacity())
          queue.push(child_l);
        uint32_t child_r = 2*node+2;
        if(child_r < queue.capacity())
          queue.push(child_r);
#ifdef DEBUG
      } else {
        printf("Failed to insert (%u,%u,%u). Already exists.\n", info.node, info.src, info.tree);
#endif
      }
    });
    q_size = queue.size();
  }

  printf("Number of comparisons (Merkle Tree): %u\n", num_comp);
  Kokkos::fence();
}

template<typename Scheduler>
struct CompareTreeTask {
  using sched_type  = Scheduler;
  using future_type = Kokkos::BasicFuture<uint32_t, Scheduler>;
  using value_type  = uint32_t;

  uint32_t node;
  uint32_t tree_id;
  MerkleTree tree;
  DistinctMap distinct_map;
  future_type child_l_fut;
  future_type child_r_fut;
  bool l_active;
  bool r_active;

  KOKKOS_INLINE_FUNCTION
  CompareTreeTask(const uint32_t n, const MerkleTree& merkle_tree, const uint32_t treeID, DistinctMap& distinct) : 
                  node(n), tree_id(treeID), tree(merkle_tree), distinct_map(distinct), l_active(true), r_active(true) {}

  KOKKOS_INLINE_FUNCTION
  void operator()(typename sched_type::member_type& member, uint32_t& result) {
    auto& sched = member.scheduler();

    bool child_l_ready = ( (l_active && !child_l_fut.is_null()) || (l_active == false) );
    bool child_r_ready = ( (r_active && !child_r_fut.is_null()) || (r_active == false) );
    if((child_l_ready && child_r_ready)) { // On task respawn
      result = 1 + child_l_fut.get() + child_r_fut.get();
    } else { // Perform task and spawn for children if needed
      uint32_t active_children = 0;
      HashDigest digest = tree(node);
      NodeInfo info(node, node, tree_id);
      auto insert_result = distinct_map.insert(digest, info); // Try to insert
      if(insert_result.success()) { // Node is distinct
        uint32_t child_l = 2*node+1;
        if(child_l < tree.tree_d.extent(0)) {
          child_l_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CompareTreeTask(child_l, tree, tree_id, distinct_map));
          active_children += 1;
        } else {
          l_active = false;
        }
        uint32_t child_r = 2*node+2;
        if(child_r < tree.tree_d.extent(0)) {
          child_r_fut = Kokkos::task_spawn(Kokkos::TaskSingle(sched, Kokkos::TaskPriority::High), CompareTreeTask(child_r, tree, tree_id, distinct_map));
          active_children += 1;
        } else {
          l_active = false;
        }
      }
      if(active_children == 2) {
        Kokkos::BasicFuture<void, Scheduler> dep[] = {child_l_fut, child_r_fut};
        Kokkos::BasicFuture<void, Scheduler> all_children = sched.when_all(dep, 2);
        Kokkos::respawn(this, all_children, Kokkos::TaskPriority::High);
      } else if(active_children == 1) {
        if(l_active) { 
          Kokkos::respawn(this, child_l_fut, Kokkos::TaskPriority::High);
        } else {
          Kokkos::respawn(this, child_r_fut, Kokkos::TaskPriority::High);
        }
      } else {
        result = 1;
      }
    }
  }
};

void compare_trees_tasks(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, DistinctMap& distinct_map) {
  using scheduler_type = Kokkos::TaskScheduler<Kokkos::DefaultExecutionSpace>;
  using memory_space = typename scheduler_type::memory_space;
  using memory_pool = typename scheduler_type::memory_pool;
  auto mpool = memory_pool(memory_space{}, estimate_required_memory(tree.tree_d.extent(0)));
  auto root_sched = scheduler_type(mpool);
  Kokkos::BasicFuture<uint32_t, scheduler_type> f = Kokkos::host_spawn(Kokkos::TaskSingle(root_sched), 
                                                                  CompareTreeTask<scheduler_type>(0, tree, tree_id, distinct_map));
  Kokkos::wait(root_sched);
  printf("Number of comparisons (Merkle Tree Task): %u\n", f.get());
}


void count_distinct_nodes(const MerkleTree& tree, Queue& queue, const uint32_t tree_id, const DistinctMap& distinct) {
  Kokkos::View<uint32_t[1]> n_distinct("Num distinct\n");
  Kokkos::View<uint32_t[1]>::HostMirror n_distinct_h = Kokkos::create_mirror_view(n_distinct);
  Kokkos::deep_copy(n_distinct, 0);
  uint32_t q_size = queue.size();
  while(q_size > 0) {
    Kokkos::parallel_for(q_size, KOKKOS_LAMBDA(const uint32_t entry) {
      uint32_t node = queue.pop();
      HashDigest digest = tree(node);
      if(distinct.exists(digest)) {
        uint32_t existing_id = distinct.find(digest);
        NodeInfo info = distinct.value_at(existing_id);
        Kokkos::atomic_add(&n_distinct(0), 1);
        if(info.node == node && info.tree == tree_id) {
          uint32_t child_l = 2*node+1;
          if(child_l < queue.capacity())
            queue.push(child_l);
          uint32_t child_r = 2*node+2;
          if(child_r < queue.capacity())
            queue.push(child_r);
	}
      } else {
        printf("Node %u digest not in map. This shouldn't happen.\n", node);
      }
    });
    q_size = queue.size();
  }
  Kokkos::deep_copy(n_distinct_h, n_distinct);
  Kokkos::fence();
  printf("Number of distinct nodes: %u out of %u\n", n_distinct_h(0), tree.tree_d.extent(0));
}

void print_nodes(const MerkleTree& tree, const uint32_t tree_id, const DistinctMap& distinct) {
  Kokkos::View<uint32_t[1]> n_distinct("Num distinct\n");
  Kokkos::View<uint32_t[1]>::HostMirror n_distinct_h = Kokkos::create_mirror_view(n_distinct);
  Kokkos::deep_copy(n_distinct, 0);
  Queue queue(tree.tree_d.extent(0));
  queue.host_push(0);
  uint32_t q_size = queue.size();
  while(q_size > 0) {
    Kokkos::parallel_for(1, KOKKOS_LAMBDA(const uint32_t entry) {
      for(uint32_t i=0; i<q_size; i++) {
        uint32_t node = queue.pop();
        HashDigest digest = tree(node);
        if(distinct.exists(digest)) {
          uint32_t existing_id = distinct.find(digest);
          NodeInfo info = distinct.value_at(existing_id);
          printf("Distinct Node %u: (%u,%u,%u)\n", node, info.node, info.src, info.tree);
          Kokkos::atomic_add(&n_distinct(0), 1);
          if(info.node == node && info.tree == tree_id) {
            uint32_t child_l = 2*node+1;
            if(child_l < queue.capacity())
              queue.push(child_l);
            uint32_t child_r = 2*node+2;
            if(child_r < queue.capacity())
              queue.push(child_r);
          }
        } else {
          printf("Node %u digest not in map. This shouldn't happen.\n", node);
        }
      }
    });
    q_size = queue.size();
  }
  Kokkos::deep_copy(n_distinct_h, n_distinct);
  Kokkos::fence();
  printf("Number of distinct nodes: %u out of %u\n", n_distinct_h(0), tree.tree_d.extent(0));
}

#endif // KOKKOS_MERKLE_TREE_HPP
