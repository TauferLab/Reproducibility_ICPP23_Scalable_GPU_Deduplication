#ifndef RESTART_MERKLE_TREE_HPP
#define RESTART_MERKLE_TREE_HPP
#include <Kokkos_Core.hpp>
#include <Kokkos_UnorderedMap.hpp>
#include <Kokkos_ScatterView.hpp>
#include <climits>
#include "hash_functions.hpp"
#include "map_helpers.hpp"
#include "kokkos_queue.hpp"
#include "kokkos_merkle_tree.hpp"
#include <iostream>
#include "utils.hpp"

std::pair<double,double>
restart_chkpt_local(std::vector<std::string>& chkpt_files, 
                             const int file_idx, 
                             std::ifstream& file,
                             Kokkos::View<uint8_t*>& data,
                             size_t filesize,
                             uint32_t num_chunks,
                             uint32_t num_nodes,
                             header_t header,
                             Kokkos::View<uint8_t*>& buffer_d,
                             Kokkos::View<uint8_t*>::HostMirror& buffer_h
                             ) {
    // Main checkpoint
    file.open(chkpt_files[file_idx], std::ifstream::in | std::ifstream::binary);
    file.read((char*)(buffer_h.data()), filesize);
    file.close();
    Kokkos::fence();
    std::chrono::high_resolution_clock::time_point c1 = std::chrono::high_resolution_clock::now();
    Kokkos::deep_copy(buffer_d, buffer_h);
    Kokkos::fence();
    std::chrono::high_resolution_clock::time_point c2 = std::chrono::high_resolution_clock::now();
    Kokkos::View<NodeID*> node_list("List of NodeIDs", num_chunks);
    Kokkos::deep_copy(node_list, NodeID());
    uint32_t ref_id = header.ref_id;
    uint32_t cur_id = header.chkpt_id;

    size_t curr_repeat_offset = sizeof(header_t) + header.distinct_size*sizeof(uint32_t);
    size_t prev_repeat_offset = curr_repeat_offset + header.curr_repeat_size*2*sizeof(uint32_t);
    size_t data_offset = prev_repeat_offset + header.prev_repeat_size*(sizeof(uint32_t)+sizeof(uint32_t));
    auto curr_repeat   = Kokkos::subview(buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
    auto prev_repeat   = Kokkos::subview(buffer_d, std::make_pair(prev_repeat_offset, data_offset));
    auto distinct = Kokkos::subview(buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
    auto data_subview = Kokkos::subview(buffer_d, std::make_pair(data_offset, filesize));
    STDOUT_PRINT("Checkpoint %u\n", header.chkpt_id);
    STDOUT_PRINT("Checkpoint size: %lu\n", filesize);
    STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
    STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
    STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
    STDOUT_PRINT("Data offset: %lu\n", data_offset);

    uint32_t chunk_size = header.chunk_size;
    size_t datalen = header.datalen;
    uint32_t num_distinct = header.distinct_size;
//    Kokkos::UnorderedMap<NodeID, size_t, Kokkos::CudaUVMSpace> distinct_map(num_nodes);
    Kokkos::UnorderedMap<NodeID, size_t, Kokkos::DefaultExecutionSpace> distinct_map(num_nodes);
    Kokkos::View<uint32_t*> distinct_nodes("Nodes", num_distinct);
    Kokkos::View<uint32_t*> chunk_len("Num chunks for node", num_distinct);
    Kokkos::parallel_for("Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
      uint32_t len = num_leaf_descendents(node, num_nodes);
      distinct_nodes(i) = node;
      chunk_len(i) = len;
    });

    Kokkos::parallel_scan("Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      const uint32_t len = chunk_len(i);
      if(is_final) chunk_len(i) = partial_sum;
      partial_sum += len;
    });

    Kokkos::parallel_for("Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      uint32_t start = leftmost_leaf(node, num_nodes);
      uint32_t len = num_leaf_descendents(node, num_nodes);
      uint32_t end = start+len-1;
      uint32_t left = 2*node+1;
      uint32_t right = 2*node+2;
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size);
      while(left < num_nodes) {
        if(right >= num_nodes)
          right = num_nodes;
        for(uint32_t u=left; u<=right; u++) {
          uint32_t leaf = leftmost_leaf(u, num_nodes);
          auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
//          DEBUG_PRINT("Inserting distinct node (%u,%u): %lu\n", u, cur_id, read_offset+sizeof(uint32_t)+(leaf-start)*chunk_size);
          if(result.failed())
            printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
        }
        left = 2*left+1;
        right = 2*right+2;
      }
      for(uint32_t j=0; j<len; j++) {
        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, UINT32_MAX);
      }
      uint32_t datasize = len*chunk_size;
      if(end == num_nodes-1)
        datasize = datalen - (start-num_chunks+1)*chunk_size;
//      DEBUG_PRINT("Copying %c to position %u, len: %u\n", (char)(*(distinct.data()+read_offset+sizeof(uint32_t))), chunk_size*(start-num_chunks+1), datasize);
      memcpy(data.data()+chunk_size*(start-num_chunks+1), data_subview.data()+chunk_len(i)*chunk_size, datasize);
    });

    Kokkos::parallel_for("Restart Hashtree current repeat", Kokkos::RangePolicy<>(0, header.curr_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      memcpy(&node, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      size_t offset = distinct_map.value_at(distinct_map.find(NodeID(prev, cur_id)));
      uint32_t node_start = leftmost_leaf(node, num_nodes);
      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
      uint32_t len = num_leaf_descendents(prev, num_nodes);
      for(uint32_t j=0; j<len; j++) {
        node_list(node_start+j-num_chunks+1) = NodeID(prev_start-num_chunks+1+j, UINT32_MAX);
//        DEBUG_PRINT("Entry %u updated: (%u,%u) (current repeat node %u prev %u)\n", node_start-num_chunks+1+j, prev_start+j, UINT32_MAX, node, prev);
      }
      uint32_t copysize = chunk_size;
      if(node == num_nodes-1)
        copysize = data.size() - chunk_size*(num_chunks-1);
      memcpy(data.data()+chunk_size*(node_start-num_chunks+1), data_subview.data()+offset, copysize);
      DEBUG_PRINT("Duplicated (%u) %c%c to %u (offset %lu)\n", node_start-num_chunks+1, *((char*)(distinct.data()+offset)), *((char*)(distinct.data()+offset+1)),  (node_start-num_chunks+1)*chunk_size, offset);
    });

    Kokkos::parallel_for("Restart Hashtree previous repeat", Kokkos::RangePolicy<>(0, header.prev_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      memcpy(&node, prev_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, prev_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)) +sizeof(uint32_t), sizeof(uint32_t));
      DEBUG_PRINT("Node: %u, Prev: %u\n", node, prev);
      uint32_t node_start = leftmost_leaf(node, num_nodes);
      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
      uint32_t len = num_leaf_descendents(prev, num_nodes);
      for(uint32_t j=0; j<len; j++) {
        node_list(node_start-num_chunks+1+j) = NodeID(prev_start+j, ref_id);
//        DEBUG_PRINT("Entry %u updated: (%u,%u) (previous repeat node %u, prev %u)\n", node_start-num_chunks+1, prev_start+j, ref_id, node, prev);
      }
    });

    Kokkos::fence();
    DEBUG_PRINT("Restarted previous repeats\n");
    Kokkos::parallel_for("Fill same entries", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
      NodeID entry = node_list(i);
      if(entry.node == UINT_MAX) {
        node_list(i) = NodeID(i+num_chunks-1, ref_id);
      }
    });
    Kokkos::fence();

    if(header.ref_id != header.chkpt_id) {
      // Reference checkpoint
      file.open(chkpt_files[header.ref_id], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
      size_t chkpt_size = file.tellg();
      file.seekg(0);
      Kokkos::View<uint8_t*> chkpt_buffer_d("Checkpoint buffer", chkpt_size);
      auto chkpt_buffer_h = Kokkos::create_mirror_view(chkpt_buffer_d);
      file.read((char*)(chkpt_buffer_h.data()), chkpt_size);
      file.close();
      header_t chkpt_header;
      memcpy(&chkpt_header, chkpt_buffer_h.data(), sizeof(header_t));
      uint32_t current_id = chkpt_header.chkpt_id;
      datalen = chkpt_header.datalen;
      chunk_size = chkpt_header.chunk_size;
      uint32_t num_distinct = chkpt_header.distinct_size;
      Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
      cur_id = chkpt_header.chkpt_id;
      ref_id = chkpt_header.ref_id;

      curr_repeat_offset = sizeof(header_t) + chkpt_header.distinct_size*sizeof(uint32_t);
      prev_repeat_offset = curr_repeat_offset + chkpt_header.curr_repeat_size*2*sizeof(uint32_t);
      data_offset = prev_repeat_offset + chkpt_header.prev_repeat_size*2*sizeof(uint32_t);
      curr_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
      prev_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(prev_repeat_offset, data_offset));
      distinct = Kokkos::subview(chkpt_buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
      data_subview = Kokkos::subview(chkpt_buffer_d, std::make_pair(data_offset, chkpt_size));
      STDOUT_PRINT("Checkpoint %u\n", chkpt_header.chkpt_id);
      STDOUT_PRINT("Checkpoint size: %lu\n", chkpt_size);
      STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
      STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
      STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
      STDOUT_PRINT("Data offset: %lu\n", data_offset);
      
      distinct_map.clear();
      distinct_map.rehash(chkpt_header.distinct_size);
      Kokkos::fence();
//      Kokkos::UnorderedMap<uint32_t, NodeID, Kokkos::CudaUVMSpace> repeat_map(chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size);
      Kokkos::UnorderedMap<uint32_t, NodeID, Kokkos::DefaultExecutionSpace> repeat_map(chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size);
      Kokkos::View<uint32_t*> distinct_nodes("Nodes", num_distinct);
      Kokkos::View<uint32_t*> chunk_len("Num chunks for node", num_distinct);
      Kokkos::parallel_for("Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node;
        memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
        uint32_t len = num_leaf_descendents(node, num_nodes);
        distinct_nodes(i) = node;
        chunk_len(i) = len;
      });
      Kokkos::parallel_scan("Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
        const uint32_t len = chunk_len(i);
        if(is_final) chunk_len(i) = partial_sum;
        partial_sum += len;
      });

      Kokkos::parallel_for("Fill distinct map", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node = distinct_nodes(i);
        distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      });

      uint32_t num_repeat = chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size;
      Kokkos::parallel_for("Fill repeat map", Kokkos::RangePolicy<>(0, num_repeat), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node;
        uint32_t prev;
        memcpy(&node, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
        memcpy(&prev, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
        repeat_map.insert(node, NodeID(prev, ref_id));
        DEBUG_PRINT("Inserting repeat node %u: (%u,%u)\n", node, prev, ref_id);
      });
      Kokkos::parallel_for("Fill data", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
        if(node_list(i).tree == current_id) {
          NodeID id = node_list(i);
          if(distinct_map.exists(id)) {
//            if(!distinct_map.valid_at(distinct_map.find(id)))
//              DEBUG_PRINT("Entry (%u,%u) not in distinct map\n", id.node, id.tree);
            size_t offset = distinct_map.value_at(distinct_map.find(id));
            uint32_t start = leftmost_leaf(id.node, num_nodes);
            uint32_t len = num_leaf_descendents(id.node, num_nodes);
            uint32_t end = start+len-1;
            uint32_t writesize = chunk_size*len;
            if(end == num_nodes-1)
              writesize = datalen-(start-num_chunks+1)*chunk_size;
            memcpy(data.data()+chunk_size*(i), data_subview.data()+offset, writesize);
            DEBUG_PRINT("Updating distinct node %u with (%u,%u). Start: %u, Len: %u, End: %u, writesize: %u, %c\n", i, id.node, id.tree, start, len, end, writesize, *((char*)(data.data()+chunk_size*i)));
          } else if(repeat_map.exists(id.node)) {
            NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
            DEBUG_PRINT("Found repeat node: %u: (%u,%u)\n", id.node, prev.node, prev.tree);
            if(prev.tree == current_id) {
              size_t offset = distinct_map.value_at(distinct_map.find(prev));
              if(!distinct_map.valid_at(distinct_map.find(prev)))
                DEBUG_PRINT("Repeated entry (%u,%u) not in distinct map\n", prev.node, prev.tree);
              uint32_t start = leftmost_leaf(prev.node, num_nodes);
              uint32_t len = num_leaf_descendents(prev.node, num_nodes);
              uint32_t writesize = chunk_size*len;
              if(datalen <= (start-num_chunks+1)*chunk_size+len*chunk_size)
                writesize = datalen-(start-num_chunks+1)*chunk_size;
              memcpy(data.data()+chunk_size*(i), data_subview.data()+offset, writesize); 
              DEBUG_PRINT("Updating repeat node %u with (%u,%u). Start: %u, Len: %u, writesize: %u, %c\n", i, id.node, id.tree, start, len, writesize, *((char*)(data.data()+chunk_size*i)));
            } else {
              node_list(i) = prev;
            }
          } else {
            node_list(i) = NodeID(node_list(i).node, current_id-1);
          }
        }
      });
    }
    Kokkos::fence();
    std::chrono::high_resolution_clock::time_point c3 = std::chrono::high_resolution_clock::now();
    double copy_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c2-c1).count());
    double restart_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c3-c2).count());
    return std::make_pair(copy_time, restart_time);
}

std::pair<double,double> 
restart_chkpt_global(std::vector<std::string>& chkpt_files, 
                             const int file_idx, 
                             std::ifstream& file,
                             Kokkos::View<uint8_t*>& data,
                             size_t filesize,
                             uint32_t num_chunks,
                             uint32_t num_nodes,
                             header_t header,
                             Kokkos::View<uint8_t*>& buffer_d,
                             Kokkos::View<uint8_t*>::HostMirror& buffer_h
                             ) {
    // Main checkpoint
//Kokkos::Profiling::pushRegion("Setup main checkpoint");
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx));
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+":Read checkpoint");
    DEBUG_PRINT("Global checkpoint\n");
    std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    Kokkos::resize(buffer_d, filesize);
    Kokkos::resize(buffer_h, filesize);
    file.open(chkpt_files[file_idx], std::ifstream::in | std::ifstream::binary);
    file.read((char*)(buffer_h.data()), filesize);
    file.close();
    std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    STDOUT_PRINT("Time spent reading checkpoint %u from file: %f\n", file_idx, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
    Kokkos::fence();
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Setup");
    std::chrono::high_resolution_clock::time_point c1 = std::chrono::high_resolution_clock::now();
    Kokkos::deep_copy(buffer_d, buffer_h);
    Kokkos::fence();
    std::chrono::high_resolution_clock::time_point c2 = std::chrono::high_resolution_clock::now();
    Kokkos::View<NodeID*> node_list("List of NodeIDs", num_chunks);
    Kokkos::deep_copy(node_list, NodeID());
    uint32_t ref_id = header.ref_id;
    uint32_t cur_id = header.chkpt_id;

    size_t curr_repeat_offset = sizeof(header_t) + header.distinct_size*sizeof(uint32_t);
    size_t prev_repeat_offset = header.num_prior_chkpts*2*sizeof(uint32_t) + curr_repeat_offset + header.curr_repeat_size*2*sizeof(uint32_t);
    size_t data_offset = prev_repeat_offset + header.prev_repeat_size*(sizeof(uint32_t)+sizeof(uint32_t));
    auto curr_repeat   = Kokkos::subview(buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
    auto prev_repeat   = Kokkos::subview(buffer_d, std::make_pair(prev_repeat_offset, data_offset));
    auto distinct      = Kokkos::subview(buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
    auto data_subview  = Kokkos::subview(buffer_d, std::make_pair(data_offset, filesize));
//    STDOUT_PRINT("Checkpoint %u\n", header.chkpt_id);
//    STDOUT_PRINT("Checkpoint size: %lu\n", filesize);
//    STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//    STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//    STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//    STDOUT_PRINT("Data offset: %lu\n", data_offset);

    uint32_t num_prior_chkpts = header.num_prior_chkpts;
    DEBUG_PRINT("Num prior checkpoints to read: %u at %lu\n", num_prior_chkpts, curr_repeat_offset);

    Kokkos::View<uint64_t[1]> counter_d("Write counter");
    auto counter_h = Kokkos::create_mirror_view(counter_d);
    Kokkos::deep_copy(counter_d, 0);

    uint32_t chunk_size = header.chunk_size;
    size_t datalen = header.datalen;
    Kokkos::UnorderedMap<NodeID, size_t> distinct_map(num_nodes);
    Kokkos::UnorderedMap<uint32_t, NodeID> repeat_map(2*num_nodes-1);
    uint32_t num_distinct = header.distinct_size;
    Kokkos::View<uint32_t*> distinct_nodes("Nodes", num_distinct);
    Kokkos::View<uint32_t*> chunk_len("Num chunks for node", num_distinct);
//Kokkos::Profiling::popRegion();
//    Kokkos::Profiling::pushRegion("Main checkpoint");
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Restart distinct");
    // Calculate sizes of each distinct region
    Kokkos::parallel_for("Tree:Main:Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
      uint32_t len = num_leaf_descendents(node, num_nodes);
      distinct_nodes(i) = node;
      chunk_len(i) = len;
      DEBUG_PRINT("Region %u, node %u with length %u\n", i, node, len);
    });
    // Perform exclusive prefix scan to determine where to write chunks for each region
    Kokkos::parallel_scan("Tree:Main:Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      const uint32_t len = chunk_len(i);
      if(is_final) chunk_len(i) = partial_sum;
      partial_sum += len;
    });

    Kokkos::View<uint32_t[1]> total_region_size("TOtal region size");
    Kokkos::View<uint32_t[1]>::HostMirror total_region_size_h = Kokkos::create_mirror_view(total_region_size);
    Kokkos::deep_copy(total_region_size, 0);

    // Restart distinct entries by reading and inserting full tree into distinct map
    Kokkos::parallel_for("Tree:Main:Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      uint32_t start = leftmost_leaf(node, num_nodes);
      uint32_t len = num_leaf_descendents(node, num_nodes);
      uint32_t end = start+len-1;
      uint32_t left = 2*node+1;
      uint32_t right = 2*node+2;
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
      while(left < num_nodes) {
        if(right >= num_nodes)
          right = num_nodes;
        for(uint32_t u=left; u<=right; u++) {
          uint32_t leaf = leftmost_leaf(u, num_nodes);
          auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
//          DEBUG_PRINT("Inserting distinct node (%u,%u): %lu\n", u, cur_id, read_offset+sizeof(uint32_t)+(leaf-start)*chunk_size);
          if(result.failed())
            printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
        }
        left = 2*left+1;
        right = 2*right+2;
      }
      // Update chunk metadata list
      for(uint32_t j=0; j<len; j++) {
        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, cur_id);
      }
      uint32_t datasize = len*chunk_size;
      if(end == num_nodes-1)
        datasize = datalen - (start-num_chunks+1)*chunk_size;
      memcpy(data.data()+chunk_size*(start-num_chunks+1), data_subview.data()+chunk_len(i)*chunk_size, datasize);
Kokkos::atomic_add(&total_region_size(0), len);
    });

Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Restart repeats");
    Kokkos::View<uint32_t*> repeat_region_sizes("Repeat entires per chkpt", cur_id+1);
    auto repeat_region_sizes_h = Kokkos::create_mirror_view(repeat_region_sizes);
    Kokkos::deep_copy(repeat_region_sizes, 0);
    // Read map of repeats for each checkpoint
    Kokkos::parallel_for("Tree:Main:Load repeat map", Kokkos::RangePolicy<>(0,num_prior_chkpts), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t chkpt;
      memcpy(&chkpt, buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t), sizeof(uint32_t));
      memcpy(&repeat_region_sizes(chkpt), buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t)+sizeof(uint32_t), sizeof(uint32_t));
      DEBUG_PRINT("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
    });
    Kokkos::deep_copy(repeat_region_sizes_h, repeat_region_sizes);
    // Perform exclusive scan to determine where regions start/stop
    Kokkos::parallel_scan("Tree:Main:Repeat offsets", cur_id+1, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      partial_sum += repeat_region_sizes(i);
      if(is_final) repeat_region_sizes(i) = partial_sum;
    });

    DEBUG_PRINT("Num repeats: %u\n", header.curr_repeat_size+header.prev_repeat_size);
    // Load repeat entries and fill in metadata for chunks
    Kokkos::parallel_for("Tree:Main:Restart Hash tree repeats main checkpoint", Kokkos::RangePolicy<>(0, header.curr_repeat_size+header.prev_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      uint32_t tree = 0;
      memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      // Determine ID 
//      for(uint32_t j=repeat_region_sizes.size()-1; j>=0 && j<repeat_region_sizes.size(); j--) {
      for(uint32_t j=repeat_region_sizes.size()-1; j<repeat_region_sizes.size(); j--) {
        if(i < repeat_region_sizes(j)) {
          tree = j;
        }
      }
      uint32_t idx = distinct_map.find(NodeID(prev, tree));
      size_t offset = distinct_map.value_at(distinct_map.find(NodeID(prev, tree)));
      uint32_t node_start = leftmost_leaf(node, num_nodes);
      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
      uint32_t len = num_leaf_descendents(prev, num_nodes);
      for(uint32_t j=0; j<len; j++) {
        node_list(node_start+j-num_chunks+1) = NodeID(prev_start+j, tree);
      }
      if(tree == cur_id) {
Kokkos::atomic_add(&total_region_size(0), len);
        uint32_t copysize = chunk_size*len;
        if(node_start+len-1 == num_nodes-1)
          copysize = data.size() - chunk_size*(node_start-num_chunks+1);
        memcpy(data.data()+chunk_size*(node_start-num_chunks+1), data_subview.data()+offset, copysize);
      }
    });
Kokkos::deep_copy(total_region_size_h, total_region_size);
DEBUG_PRINT("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));

Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Fill repeats");
    // All remaining entries are identical 
    Kokkos::parallel_for("Tree:Main:Fill same entries", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
      NodeID entry = node_list(i);
      if(entry.node == UINT_MAX) {
        node_list(i) = NodeID(i+num_chunks-1, cur_id-1);
      }
    });
    Kokkos::fence();
//    Kokkos::Profiling::popRegion();
Kokkos::Profiling::popRegion();
Kokkos::Profiling::popRegion();

    for(int idx=static_cast<int>(file_idx)-1; idx>static_cast<int>(ref_id); idx--) {
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx));
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+":Read checkpoint");
      DEBUG_PRINT("Processing checkpoint %u\n", idx);
      t1 = std::chrono::high_resolution_clock::now();
      file.open(chkpt_files[idx], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
      size_t chkpt_size = file.tellg();
//      STDOUT_PRINT("Checkpoint size: %zd\n", chkpt_size);
      file.seekg(0);
      auto chkpt_buffer_d = buffer_d;
      auto chkpt_buffer_h = buffer_h;
      Kokkos::resize(chkpt_buffer_d, chkpt_size);
      Kokkos::resize(chkpt_buffer_h, chkpt_size);
//      Kokkos::View<uint8_t*> chkpt_buffer_d("Checkpoint buffer", chkpt_size);
//      auto chkpt_buffer_h = Kokkos::create_mirror_view(chkpt_buffer_d);
      file.read((char*)(chkpt_buffer_h.data()), chkpt_size);
      file.close();
      t2 = std::chrono::high_resolution_clock::now();
      STDOUT_PRINT("Time spent reading checkpoint %d from file: %f\n", idx, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Setup");
      header_t chkpt_header;
      memcpy(&chkpt_header, chkpt_buffer_h.data(), sizeof(header_t));
      uint32_t current_id = chkpt_header.chkpt_id;
      datalen = chkpt_header.datalen;
      chunk_size = chkpt_header.chunk_size;
//      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Copy to GPU");
      Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
//      Kokkos::Profiling::popRegion();
      ref_id = chkpt_header.ref_id;
      cur_id = chkpt_header.chkpt_id;
//      STDOUT_PRINT("Cur ID: %u\n", cur_id);
//      STDOUT_PRINT("Ref ID: %u\n", ref_id);
//      STDOUT_PRINT("Num distinct: %u\n", chkpt_header.distinct_size);
//      STDOUT_PRINT("Num current: %u\n", chkpt_header.curr_repeat_size);
//      STDOUT_PRINT("Num previous: %u\n", chkpt_header.prev_repeat_size);
      num_distinct = chkpt_header.distinct_size;

//      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Clear and reset distinct/repeat maps");
      distinct_map.clear();
//      distinct_map.rehash(num_nodes);
      repeat_map.clear();
//      Kokkos::UnorderedMap<uint32_t, NodeID> repeat_map(2*num_nodes-1);
//      Kokkos::Profiling::popRegion();

      curr_repeat_offset = sizeof(header_t) + chkpt_header.distinct_size*sizeof(uint32_t);
      prev_repeat_offset = chkpt_header.num_prior_chkpts*2*sizeof(uint32_t) + curr_repeat_offset + chkpt_header.curr_repeat_size*2*sizeof(uint32_t);
      data_offset = prev_repeat_offset + chkpt_header.prev_repeat_size*(sizeof(uint32_t)+sizeof(uint32_t));
      curr_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
      prev_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(prev_repeat_offset, data_offset));
      distinct      = Kokkos::subview(chkpt_buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
      data_subview  = Kokkos::subview(chkpt_buffer_d, std::make_pair(data_offset, filesize));
//      STDOUT_PRINT("Checkpoint %u\n", chkpt_header.chkpt_id);
//      STDOUT_PRINT("Checkpoint size: %lu\n", filesize);
//      STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//      STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//      STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//      STDOUT_PRINT("Data offset: %lu\n", data_offset);

      uint32_t num_prior_chkpts = chkpt_header.num_prior_chkpts;
      
      Kokkos::View<uint64_t[1]> counter_d("Write counter");
      auto counter_h = Kokkos::create_mirror_view(counter_d);
      Kokkos::deep_copy(counter_d, 0);
  
//      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Resize");
      Kokkos::resize(distinct_nodes, num_distinct);
      Kokkos::resize(chunk_len, num_distinct);
//      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Load maps");
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node;
        memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
        uint32_t len = num_leaf_descendents(node, num_nodes);
        distinct_nodes(i) = node;
        chunk_len(i) = len;
      });
      Kokkos::parallel_scan("Tree:"+std::to_string(idx)+":Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
        const uint32_t len = chunk_len(i);
        if(is_final) chunk_len(i) = partial_sum;
        partial_sum += len;
      });
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node = distinct_nodes(i);
        distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
        uint32_t start = leftmost_leaf(node, num_nodes);
        uint32_t len = num_leaf_descendents(node, num_nodes);
        uint32_t left = 2*node+1;
        uint32_t right = 2*node+2;
        DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
        while(left < num_nodes) {
          if(right >= num_nodes)
            right = num_nodes;
          for(uint32_t u=left; u<=right; u++) {
            uint32_t leaf = leftmost_leaf(u, num_nodes);
            auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
            if(result.failed())
              printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
          }
          left = 2*left+1;
          right = 2*right+2;
        }
      });
  
//printf("Chkpt: %u, num_distinct: %u\n", cur_id, num_distinct);
      Kokkos::View<uint32_t*> repeat_region_sizes("Repeat entires per chkpt", cur_id+1);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Load repeat map", Kokkos::RangePolicy<>(0,num_prior_chkpts), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t chkpt;
        memcpy(&chkpt, chkpt_buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t), sizeof(uint32_t));
        memcpy(&repeat_region_sizes(chkpt), chkpt_buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t)+sizeof(uint32_t), sizeof(uint32_t));
        DEBUG_PRINT("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
//printf("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
      });
      Kokkos::parallel_scan("Tree:"+std::to_string(idx)+":Repeat offsets", cur_id+1, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
        partial_sum += repeat_region_sizes(i);
        if(is_final) repeat_region_sizes(i) = partial_sum;
      });

      DEBUG_PRINT("Num repeats: %u\n", chkpt_header.curr_repeat_size+chkpt_header.prev_repeat_size);
  
      uint32_t num_repeat_entries = chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size;

      Kokkos::TeamPolicy<> repeat_policy(num_repeat_entries, Kokkos::AUTO);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Restart Hash tree repeats middle chkpts", repeat_policy, 
                           KOKKOS_LAMBDA(const Kokkos::TeamPolicy<>::member_type& team_member) {
        uint32_t i = team_member.league_rank();
        uint32_t node, prev, tree=0;
        memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
        memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
        for(uint32_t j=repeat_region_sizes.size()-1; j<repeat_region_sizes.size(); j--) {
          if(i < repeat_region_sizes(j)) {
            tree = j;
          }
        }
        if(team_member.team_rank() == 0) {
          auto result = repeat_map.insert(node, NodeID(prev,tree));
          if(result.failed())
            STDOUT_PRINT("Failed to insert previous repeat %u: (%u,%u) into repeat map\n", node, prev, tree);
        }
        uint32_t curr_start = leftmost_leaf(node, num_nodes);
        uint32_t prev_start = leftmost_leaf(prev, num_nodes);
        uint32_t len = num_leaf_descendents(node, num_nodes);
        Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, len), [&] (const uint32_t u) {
          repeat_map.insert(curr_start+u, NodeID(prev_start+u, tree));
        });
      });
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Fill chunks");

Kokkos::View<uint32_t[1]> curr_identical_counter("Num identical entries in current checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror curr_identical_counter_h = Kokkos::create_mirror_view(curr_identical_counter);
Kokkos::deep_copy(curr_identical_counter, 0);
Kokkos::View<uint32_t[1]> prev_identical_counter("Num identical entries in previous checkpoint");
Kokkos::View<uint32_t[1]> base_identical_counter("Num identical entries in baseline checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror prev_identical_counter_h = Kokkos::create_mirror_view(prev_identical_counter);;
Kokkos::View<uint32_t[1]>::HostMirror base_identical_counter_h = Kokkos::create_mirror_view(base_identical_counter);;
Kokkos::deep_copy(prev_identical_counter, 0);
Kokkos::deep_copy(base_identical_counter, 0);
Kokkos::deep_copy(total_region_size, 0);
Kokkos::View<uint32_t[1]> curr_chunks("Num identical entries in current checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror curr_chunks_h = Kokkos::create_mirror_view(curr_chunks);
Kokkos::deep_copy(curr_chunks, 0);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Fill data middle chkpts", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
        if(node_list(i).tree == current_id) {
Kokkos::atomic_add(&curr_chunks(0), 1);
          NodeID id = node_list(i);
          if(distinct_map.exists(id)) {
            uint32_t len = num_leaf_descendents(id.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
            uint32_t start = leftmost_leaf(id.node, num_nodes);
            uint32_t end = rightmost_leaf(id.node, num_nodes);
            size_t offset = distinct_map.value_at(distinct_map.find(id));
            uint32_t writesize = chunk_size;
            if(i*chunk_size+writesize > datalen) 
              writesize = datalen-i*chunk_size;
            memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
            Kokkos::atomic_add(&counter_d(0), writesize);
Kokkos::atomic_add(&curr_identical_counter(0), writesize);
          } else if(repeat_map.exists(id.node)) {
            NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
            DEBUG_PRINT("Repaeat value: %u: (%u,%u)\n", id.node, prev.node, prev.tree);
            if(prev.tree == current_id) {
              if(!repeat_map.exists(id.node))
                printf("Failed to find repeat chunk %u\n", id.node);
              size_t offset = distinct_map.value_at(distinct_map.find(prev));
              uint32_t len = num_leaf_descendents(prev.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
              uint32_t start = leftmost_leaf(prev.node, num_nodes);
              uint32_t writesize = chunk_size;
              if(i*chunk_size+writesize > datalen) 
                writesize = datalen-i*chunk_size;
              memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
              Kokkos::atomic_add(&counter_d(0), writesize);
Kokkos::atomic_add(&curr_identical_counter(0), writesize);
            } else {
              node_list(i) = prev;
//if(prev.tree == 0)
//Kokkos::atomic_add(&base_identical_counter(0), 1);
//Kokkos::atomic_add(&prev_identical_counter(0), 1);
            }
          } else {
Kokkos::atomic_add(&prev_identical_counter(0), 1);
            node_list(i) = NodeID(node_list(i).node, current_id-1);
          }
//Kokkos::atomic_add(&curr_identical_counter(0), 1);
        } else if(node_list(i).tree < current_id) {
Kokkos::atomic_add(&base_identical_counter(0), 1);
//        } else if(node_list(i).tree < current_id) {
//Kokkos::atomic_add(&prev_identical_counter(0), 1);
        }
      });
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::popRegion();
//Kokkos::deep_copy(total_region_size_h, total_region_size);
//printf("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));
//Kokkos::deep_copy(base_identical_counter_h, base_identical_counter);
//Kokkos::deep_copy(prev_identical_counter_h, prev_identical_counter);
//Kokkos::deep_copy(curr_identical_counter_h, curr_identical_counter);
//Kokkos::deep_copy(curr_chunks_h, curr_chunks);
//printf("Number of chunks to test: %u\n", curr_chunks_h(0));
//printf("Number of bytes written for chkpt %u: %u\n", current_id, curr_identical_counter_h(0));
//printf("Number of identical chunks for chkps prior to %u: %u\n", current_id, prev_identical_counter_h(0));
//printf("Number of chunks for prior checkpoints: %u\n", base_identical_counter_h(0));
    }

    // Reference
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id));
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+":Read checkpoint");
    t1 = std::chrono::high_resolution_clock::now();
    file.open(chkpt_files[header.ref_id], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
    size_t chkpt_size = file.tellg();
    file.seekg(0);
    auto chkpt_buffer_d = buffer_d;
    auto chkpt_buffer_h = buffer_h;
    Kokkos::resize(chkpt_buffer_d, chkpt_size);
    Kokkos::resize(chkpt_buffer_h, chkpt_size);
//    Kokkos::View<uint8_t*> chkpt_buffer_d("Checkpoint buffer", chkpt_size);
//    auto chkpt_buffer_h = Kokkos::create_mirror_view(chkpt_buffer_d);
    file.read((char*)(chkpt_buffer_h.data()), chkpt_size);
    file.close();
    t2 = std::chrono::high_resolution_clock::now();
    STDOUT_PRINT("Time spent reading checkpoint %u from file: %f\n", header.ref_id, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
//    printf("Time spent reading checkpoint %u from file: %f\n", header.ref_id, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Setup");
    header_t chkpt_header;
    memcpy(&chkpt_header, chkpt_buffer_h.data(), sizeof(header_t));
    uint32_t current_id = chkpt_header.chkpt_id;
    datalen = chkpt_header.datalen;
    chunk_size = chkpt_header.chunk_size;
    Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
    ref_id = chkpt_header.ref_id;
    cur_id = chkpt_header.chkpt_id;

    curr_repeat_offset = sizeof(header_t) + chkpt_header.distinct_size*sizeof(uint32_t);
    prev_repeat_offset = curr_repeat_offset + chkpt_header.curr_repeat_size*2*sizeof(uint32_t);
    data_offset = prev_repeat_offset + chkpt_header.prev_repeat_size*2*sizeof(uint32_t);
    curr_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
    prev_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(prev_repeat_offset, data_offset));
    distinct      = Kokkos::subview(chkpt_buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
    data_subview = Kokkos::subview(chkpt_buffer_d, std::make_pair(data_offset, chkpt_size));
//    STDOUT_PRINT("Checkpoint %u\n", chkpt_header.chkpt_id);
//    STDOUT_PRINT("Checkpoint size: %lu\n", chkpt_size);
//    STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//    STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//    STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//    STDOUT_PRINT("Data offset: %lu\n", data_offset);
    
    distinct_map.clear();
    distinct_map.rehash(chkpt_header.distinct_size);
    Kokkos::fence();
    repeat_map.clear();
//    Kokkos::UnorderedMap<uint32_t, NodeID> repeat_map(chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size);
    num_distinct = chkpt_header.distinct_size;
    Kokkos::resize(distinct_nodes, num_distinct);
    Kokkos::resize(chunk_len, num_distinct);
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Load maps");
    Kokkos::parallel_for("Tree:Reference:Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
      uint32_t len = num_leaf_descendents(node, num_nodes);
      distinct_nodes(i) = node;
      chunk_len(i) = len;
    });
    Kokkos::parallel_scan("Tree:Reference:Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      const uint32_t len = chunk_len(i);
      if(is_final) chunk_len(i) = partial_sum;
      partial_sum += len;
    });

    Kokkos::parallel_for("Tree:Reference:Fill distinct map", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size);
    });

    uint32_t num_repeat = chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size;
    Kokkos::parallel_for("Tree:Reference:Fill repeat map", Kokkos::RangePolicy<>(0, num_repeat), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      memcpy(&node, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      auto result = repeat_map.insert(node, NodeID(prev, ref_id));
      if(result.failed())
        STDOUT_PRINT("Reference: Failed to insert repeat node %u: (%u,%u)\n", node, prev, ref_id);
      DEBUG_PRINT("Reference: Inserted repeat node %u: (%u,%u): %lu\n", node, prev, ref_id, i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t));
    });
Kokkos::deep_copy(total_region_size, 0);
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Fill chunks");
    Kokkos::parallel_for("Tree:Reference:Fill data reference chkpt", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
      if(node_list(i).tree == current_id) {
        NodeID id = node_list(i);
        if(distinct_map.exists(id)) {
          uint32_t len = num_leaf_descendents(id.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
          uint32_t start = leftmost_leaf(id.node, num_nodes);
          size_t offset = distinct_map.value_at(distinct_map.find(id));
          uint32_t writesize = chunk_size*len;
          Kokkos::atomic_add(&counter_d(0), writesize);
          if((start+len-1-num_chunks+1)*chunk_size >= datalen)
            writesize = datalen-(start-num_chunks+1)*chunk_size;
          memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
        } else if(repeat_map.exists(id.node)) {
          NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
          if(prev.tree == current_id) {
            size_t offset = distinct_map.value_at(distinct_map.find(prev));
            
            uint32_t len = num_leaf_descendents(prev.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
            uint32_t start = leftmost_leaf(prev.node, num_nodes);
            uint32_t writesize = chunk_size*len;
            Kokkos::atomic_add(&counter_d(0), writesize);
            if((start+len-1-num_chunks+1)*chunk_size >= datalen)
              writesize = datalen-(start-num_chunks+1)*chunk_size;

            memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize); 
          } else {
            node_list(i) = prev;
          }
        } else {
          node_list(i) = NodeID(node_list(i).node, current_id-1);
        }
      }
    });
//Kokkos::deep_copy(total_region_size_h, total_region_size);
//printf("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));
//printf("Chkpt %u: avg region size: %u\n", cur_id, total_region_size_h(0));
    Kokkos::fence();
Kokkos::Profiling::popRegion();
Kokkos::Profiling::popRegion();
    std::chrono::high_resolution_clock::time_point c3 = std::chrono::high_resolution_clock::now();
    double copy_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c2-c1).count());
    double restart_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c3-t0).count());
    return std::make_pair(copy_time, restart_time);
}

std::pair<double,double> 
restart_chkpt_global(std::vector<std::string>& chkpt_files, 
                             const int file_idx, 
                             std::vector<Kokkos::View<uint8_t*>> buffers_d,
                             std::vector<Kokkos::View<uint8_t*>::HostMirror> buffers_h,
                             Kokkos::View<uint8_t*>& data,
                             size_t filesize,
                             uint32_t num_chunks,
                             uint32_t num_nodes,
                             header_t header
                             ) {
    // Main checkpoint
//Kokkos::Profiling::pushRegion("Setup main checkpoint");
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx));
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+":Read checkpoint");
    DEBUG_PRINT("Global checkpoint\n");
    std::chrono::high_resolution_clock::time_point t0 = std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
    auto& buffer_d = buffers_d[file_idx];
    auto& buffer_h = buffers_h[file_idx];
    Kokkos::deep_copy(buffer_d, buffer_h);
    Kokkos::fence();
//    Kokkos::resize(buffer_d, filesize);
//    Kokkos::resize(buffer_h, filesize);
//    file.open(chkpt_files[file_idx], std::ifstream::in | std::ifstream::binary);
//    file.read((char*)(buffer_h.data()), filesize);
//    file.close();
    std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now();
    STDOUT_PRINT("Time spent reading checkpoint %u from file: %f\n", file_idx, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
//    printf("Time spent reading checkpoint %u from file: %f\n", file_idx, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
    Kokkos::fence();
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Setup");
    std::chrono::high_resolution_clock::time_point c1 = std::chrono::high_resolution_clock::now();
//    Kokkos::deep_copy(buffer_d, buffer_h);
//    Kokkos::fence();
    std::chrono::high_resolution_clock::time_point c2 = std::chrono::high_resolution_clock::now();
    Kokkos::View<NodeID*> node_list("List of NodeIDs", num_chunks);
    Kokkos::deep_copy(node_list, NodeID());
    uint32_t ref_id = header.ref_id;
    uint32_t cur_id = header.chkpt_id;

    size_t curr_repeat_offset = sizeof(header_t) + header.distinct_size*sizeof(uint32_t);
    size_t prev_repeat_offset = header.num_prior_chkpts*2*sizeof(uint32_t) + curr_repeat_offset + header.curr_repeat_size*2*sizeof(uint32_t);
    size_t data_offset = prev_repeat_offset + header.prev_repeat_size*(sizeof(uint32_t)+sizeof(uint32_t));
    auto curr_repeat   = Kokkos::subview(buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
    auto prev_repeat   = Kokkos::subview(buffer_d, std::make_pair(prev_repeat_offset, data_offset));
    auto distinct      = Kokkos::subview(buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
    auto data_subview  = Kokkos::subview(buffer_d, std::make_pair(data_offset, filesize));
//    STDOUT_PRINT("Checkpoint %u\n", header.chkpt_id);
//    STDOUT_PRINT("Checkpoint size: %lu\n", filesize);
//    STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//    STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//    STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//    STDOUT_PRINT("Data offset: %lu\n", data_offset);

    uint32_t num_prior_chkpts = header.num_prior_chkpts;
    DEBUG_PRINT("Num prior checkpoints to read: %u at %lu\n", num_prior_chkpts, curr_repeat_offset);

    Kokkos::View<uint64_t[1]> counter_d("Write counter");
    auto counter_h = Kokkos::create_mirror_view(counter_d);
    Kokkos::deep_copy(counter_d, 0);

    uint32_t chunk_size = header.chunk_size;
    size_t datalen = header.datalen;
    Kokkos::UnorderedMap<NodeID, size_t> distinct_map(num_nodes);
    Kokkos::UnorderedMap<uint32_t, NodeID> repeat_map(2*num_nodes-1);
    uint32_t num_distinct = header.distinct_size;
//printf("Num distinct: %u\n", num_distinct);
    Kokkos::View<uint32_t*> distinct_nodes("Nodes", num_distinct);
    Kokkos::View<uint32_t*> chunk_len("Num chunks for node", num_distinct);
//Kokkos::Profiling::popRegion();
//    Kokkos::Profiling::pushRegion("Main checkpoint");
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Load maps");
    // Calculate sizes of each distinct region
    Kokkos::parallel_for("Tree:Main:Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
      uint32_t len = num_leaf_descendents(node, num_nodes);
      distinct_nodes(i) = node;
      chunk_len(i) = len;
      DEBUG_PRINT("Region %u, node %u with length %u\n", i, node, len);
    });
    // Perform exclusive prefix scan to determine where to write chunks for each region
    Kokkos::parallel_scan("Tree:Main:Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      const uint32_t len = chunk_len(i);
      if(is_final) chunk_len(i) = partial_sum;
      partial_sum += len;
    });

    Kokkos::View<uint32_t[1]> total_region_size("TOtal region size");
    Kokkos::View<uint32_t[1]>::HostMirror total_region_size_h = Kokkos::create_mirror_view(total_region_size);
    Kokkos::deep_copy(total_region_size, 0);

    // Restart distinct entries by reading and inserting full tree into distinct map
    Kokkos::parallel_for("Tree:Main:Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      uint32_t start = leftmost_leaf(node, num_nodes);
      uint32_t len = num_leaf_descendents(node, num_nodes);
      uint32_t left = 2*node+1;
      uint32_t right = 2*node+2;
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
      while(left < num_nodes) {
        if(right >= num_nodes)
          right = num_nodes;
        for(uint32_t u=left; u<=right; u++) {
          uint32_t leaf = leftmost_leaf(u, num_nodes);
          auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
//          DEBUG_PRINT("Inserting distinct node (%u,%u): %lu\n", u, cur_id, read_offset+sizeof(uint32_t)+(leaf-start)*chunk_size);
          if(result.failed())
            printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
        }
        left = 2*left+1;
        right = 2*right+2;
      }
      // Update chunk metadata list
      for(uint32_t j=0; j<len; j++) {
        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, cur_id);
      }
//      uint32_t datasize = len*chunk_size;
//      if(end == num_nodes-1)
//        datasize = datalen - (start-num_chunks+1)*chunk_size;
//      memcpy(data.data()+chunk_size*(start-num_chunks+1), data_subview.data()+chunk_len(i)*chunk_size, datasize);
//Kokkos::atomic_add(&total_region_size(0), len);
    });

    Kokkos::View<uint32_t*> repeat_region_sizes("Repeat entires per chkpt", cur_id+1);
    auto repeat_region_sizes_h = Kokkos::create_mirror_view(repeat_region_sizes);
    Kokkos::deep_copy(repeat_region_sizes, 0);
    // Read map of repeats for each checkpoint
    Kokkos::parallel_for("Tree:Main:Load repeat map", Kokkos::RangePolicy<>(0,num_prior_chkpts), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t chkpt;
      memcpy(&chkpt, buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t), sizeof(uint32_t));
      memcpy(&repeat_region_sizes(chkpt), buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t)+sizeof(uint32_t), sizeof(uint32_t));
      DEBUG_PRINT("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
    });
    Kokkos::deep_copy(repeat_region_sizes_h, repeat_region_sizes);
    // Perform exclusive scan to determine where regions start/stop
    Kokkos::parallel_scan("Tree:Main:Repeat offsets", cur_id+1, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      partial_sum += repeat_region_sizes(i);
      if(is_final) repeat_region_sizes(i) = partial_sum;
    });

    DEBUG_PRINT("Num repeats: %u\n", header.curr_repeat_size+header.prev_repeat_size);
    // Load repeat entries and fill in metadata for chunks
    Kokkos::parallel_for("Tree:Main:Restart Hash tree repeats main checkpoint", Kokkos::RangePolicy<>(0, header.curr_repeat_size+header.prev_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      uint32_t tree = 0;
      memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      // Determine ID 
      for(uint32_t j=repeat_region_sizes.size()-1; j<repeat_region_sizes.size(); j--) {
        if(i < repeat_region_sizes(j)) {
          tree = j;
        }
      }
      uint32_t idx = distinct_map.find(NodeID(prev, tree));
      size_t offset = distinct_map.value_at(distinct_map.find(NodeID(prev, tree)));
      uint32_t node_start = leftmost_leaf(node, num_nodes);
      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
      uint32_t len = num_leaf_descendents(prev, num_nodes);
      for(uint32_t j=0; j<len; j++) {
        node_list(node_start+j-num_chunks+1) = NodeID(prev_start+j, tree);
      }
//      if(tree == cur_id) {
//Kokkos::atomic_add(&total_region_size(0), len);
//        uint32_t copysize = chunk_size*len;
//        if(node_start+len-1 == num_nodes-1)
//          copysize = data.size() - chunk_size*(node_start-num_chunks+1);
//        memcpy(data.data()+chunk_size*(node_start-num_chunks+1), data_subview.data()+offset, copysize);
//      }
    });

Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Fill chunks");
    Kokkos::parallel_for("Tree:Main:Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
//      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      uint32_t start = leftmost_leaf(node, num_nodes);
      uint32_t len = num_leaf_descendents(node, num_nodes);
      uint32_t end = start+len-1;
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
//      while(left < num_nodes) {
//        if(right >= num_nodes)
//          right = num_nodes;
//        for(uint32_t u=left; u<=right; u++) {
//          uint32_t leaf = leftmost_leaf(u, num_nodes);
//          auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
////          DEBUG_PRINT("Inserting distinct node (%u,%u): %lu\n", u, cur_id, read_offset+sizeof(uint32_t)+(leaf-start)*chunk_size);
//          if(result.failed())
//            printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
//        }
//        left = 2*left+1;
//        right = 2*right+2;
//      }
//      // Update chunk metadata list
//      for(uint32_t j=0; j<len; j++) {
//        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, cur_id);
//      }
      uint32_t datasize = len*chunk_size;
      if(end == num_nodes-1)
        datasize = datalen - (start-num_chunks+1)*chunk_size;
      memcpy(data.data()+chunk_size*(start-num_chunks+1), data_subview.data()+chunk_len(i)*chunk_size, datasize);
Kokkos::atomic_add(&total_region_size(0), len);
    });
    Kokkos::parallel_for("Tree:Main:Restart Hash tree repeats main checkpoint", Kokkos::RangePolicy<>(0, header.curr_repeat_size+header.prev_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      uint32_t tree = 0;
      memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      // Determine ID 
      for(uint32_t j=repeat_region_sizes.size()-1; j<repeat_region_sizes.size(); j--) {
        if(i < repeat_region_sizes(j)) {
          tree = j;
        }
      }
      uint32_t idx = distinct_map.find(NodeID(prev, tree));
      size_t offset = distinct_map.value_at(distinct_map.find(NodeID(prev, tree)));
      uint32_t node_start = leftmost_leaf(node, num_nodes);
      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
      uint32_t len = num_leaf_descendents(prev, num_nodes);
//      for(uint32_t j=0; j<len; j++) {
//        node_list(node_start+j-num_chunks+1) = NodeID(prev_start+j, tree);
//      }
      if(tree == cur_id) {
Kokkos::atomic_add(&total_region_size(0), len);
        uint32_t copysize = chunk_size*len;
        if(node_start+len-1 == num_nodes-1)
          copysize = data.size() - chunk_size*(node_start-num_chunks+1);
        memcpy(data.data()+chunk_size*(node_start-num_chunks+1), data_subview.data()+offset, copysize);
      }
    });

//==============================================================================

//    // Restart distinct entries by reading and inserting full tree into distinct map
//    Kokkos::parallel_for("Tree:Main:Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
//      uint32_t node = distinct_nodes(i);
//      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
//      uint32_t start = leftmost_leaf(node, num_nodes);
//      uint32_t len = num_leaf_descendents(node, num_nodes);
//      uint32_t end = start+len-1;
//      uint32_t left = 2*node+1;
//      uint32_t right = 2*node+2;
//      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
//      while(left < num_nodes) {
//        if(right >= num_nodes)
//          right = num_nodes;
//        for(uint32_t u=left; u<=right; u++) {
//          uint32_t leaf = leftmost_leaf(u, num_nodes);
//          auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
////          DEBUG_PRINT("Inserting distinct node (%u,%u): %lu\n", u, cur_id, read_offset+sizeof(uint32_t)+(leaf-start)*chunk_size);
//          if(result.failed())
//            printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
//        }
//        left = 2*left+1;
//        right = 2*right+2;
//      }
//      // Update chunk metadata list
//      for(uint32_t j=0; j<len; j++) {
//        auto result = distinct_map.insert(NodeID(start-num_chunks+1+j, cur_id), chunk_len(i)*chunk_size + j*chunk_size);
//        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, cur_id);
////        node_list(start-num_chunks+1+j) = NodeID(start-num_chunks+1+j, cur_id);
//      }
//    });
//
//    Kokkos::View<uint32_t*> repeat_region_sizes("Repeat entires per chkpt", cur_id+1);
//    auto repeat_region_sizes_h = Kokkos::create_mirror_view(repeat_region_sizes);
//    Kokkos::deep_copy(repeat_region_sizes, 0);
//    // Read map of repeats for each checkpoint
//    Kokkos::parallel_for("Tree:Main:Load repeat map", Kokkos::RangePolicy<>(0,num_prior_chkpts), KOKKOS_LAMBDA(const uint32_t i) {
//      uint32_t chkpt;
//      memcpy(&chkpt, buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t), sizeof(uint32_t));
//      memcpy(&repeat_region_sizes(chkpt), buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t)+sizeof(uint32_t), sizeof(uint32_t));
//      DEBUG_PRINT("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
//printf("Chkpt: %u, num regions: %u\n", chkpt, repeat_region_sizes(chkpt));
//    });
//    Kokkos::deep_copy(repeat_region_sizes_h, repeat_region_sizes);
//    // Perform exclusive scan to determine where regions start/stop
//    Kokkos::parallel_scan("Tree:Main:Repeat offsets", cur_id+1, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
//      partial_sum += repeat_region_sizes(i);
//      if(is_final) repeat_region_sizes(i) = partial_sum;
//    });
//
//    DEBUG_PRINT("Num repeats: %u\n", header.curr_repeat_size+header.prev_repeat_size);
//    // Load repeat entries and fill in metadata for chunks
//    Kokkos::parallel_for("Tree:Main:Restart Hash tree repeats main checkpoint", Kokkos::RangePolicy<>(0, header.curr_repeat_size+header.prev_repeat_size), KOKKOS_LAMBDA(const uint32_t i) {
//      uint32_t node;
//      uint32_t prev;
//      uint32_t tree = 0;
//      memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
//      memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
//      // Determine ID 
//      for(uint32_t j=repeat_region_sizes.size()-1; j>=0 && j<repeat_region_sizes.size(); j--) {
//        if(i < repeat_region_sizes(j)) {
//          tree = j;
//        }
//      }
//      auto result = repeat_map.insert(node, NodeID(prev,tree));
//      uint32_t node_start = leftmost_leaf(node, num_nodes);
//      uint32_t prev_start = leftmost_leaf(prev, num_nodes);
//      uint32_t len = num_leaf_descendents(prev, num_nodes);
//      for(uint32_t j=0; j<len; j++) {
//        repeat_map.insert(node_start+j, NodeID(prev_start+j, tree));
//        node_list(node_start+j-num_chunks+1) = NodeID(prev_start+j, tree);
//      }
//    });
//Kokkos::Profiling::popRegion();
//Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(file_idx)+" Fill chunks");
//      Kokkos::parallel_for("Tree:Main:Fill data middle chkpts", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
//        if(node_list(i).tree == cur_id) {
//          NodeID id = node_list(i);
//          if(distinct_map.exists(id)) {
//            uint32_t len = num_leaf_descendents(id.node, num_nodes);
//Kokkos::atomic_add(&total_region_size(0), len);
//            uint32_t start = leftmost_leaf(id.node, num_nodes);
//            uint32_t end = rightmost_leaf(id.node, num_nodes);
//            size_t offset = distinct_map.value_at(distinct_map.find(id));
//            uint32_t writesize = chunk_size;
//            if(i*chunk_size+writesize > datalen) 
//              writesize = datalen-i*chunk_size;
//            memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
//            Kokkos::atomic_add(&counter_d(0), writesize);
//          } else if(repeat_map.exists(id.node)) {
//            NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
//            DEBUG_PRINT("Repaeat value: %u: (%u,%u)\n", id.node, prev.node, prev.tree);
//            if(prev.tree == cur_id) {
//              size_t offset = distinct_map.value_at(distinct_map.find(prev));
//              uint32_t len = num_leaf_descendents(prev.node, num_nodes);
//Kokkos::atomic_add(&total_region_size(0), len);
//              uint32_t start = leftmost_leaf(prev.node, num_nodes);
//              uint32_t writesize = chunk_size;
//              if(i*chunk_size+writesize > datalen) 
//                writesize = datalen-i*chunk_size;
//              memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
//              Kokkos::atomic_add(&counter_d(0), writesize);
//            } else {
//              node_list(i) = prev;
//            }
//          } else {
//            node_list(i) = NodeID(node_list(i).node, cur_id-1);
//          }
//        }
//      });
    // All remaining entries are identical 
    Kokkos::parallel_for("Tree:Main:Fill same entries", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
      NodeID entry = node_list(i);
      if(entry.node == UINT_MAX) {
        node_list(i) = NodeID(i+num_chunks-1, cur_id-1);
      }
    });
    Kokkos::fence();
Kokkos::Profiling::popRegion();
//Kokkos::deep_copy(total_region_size_h, total_region_size);
//printf("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));

Kokkos::Profiling::popRegion();

    for(int idx=static_cast<int>(file_idx)-1; idx>static_cast<int>(ref_id); idx--) {
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx));
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+":Read checkpoint");
      DEBUG_PRINT("Processing checkpoint %u\n", idx);
      t1 = std::chrono::high_resolution_clock::now();
//      file.open(chkpt_files[idx], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
//      size_t chkpt_size = file.tellg();
//      STDOUT_PRINT("Checkpoint size: %zd\n", chkpt_size);
//      file.seekg(0);
//      auto chkpt_buffer_d = buffer_d;
//      auto chkpt_buffer_h = buffer_h;
//      Kokkos::resize(chkpt_buffer_d, chkpt_size);
//      Kokkos::resize(chkpt_buffer_h, chkpt_size);
      auto& chkpt_buffer_d = buffers_d[idx];
      auto& chkpt_buffer_h = buffers_h[idx];
      size_t chkpt_size = chkpt_buffer_d.size();
      Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
//      Kokkos::View<uint8_t*> chkpt_buffer_d("Checkpoint buffer", chkpt_size);
//      auto chkpt_buffer_h = Kokkos::create_mirror_view(chkpt_buffer_d);
//      file.read((char*)(chkpt_buffer_h.data()), chkpt_size);
//      file.close();
      t2 = std::chrono::high_resolution_clock::now();
      STDOUT_PRINT("Time spent reading checkpoint %d from file: %f\n", idx, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Setup");
      header_t chkpt_header;
      memcpy(&chkpt_header, chkpt_buffer_h.data(), sizeof(header_t));
      uint32_t current_id = chkpt_header.chkpt_id;
      datalen = chkpt_header.datalen;
      chunk_size = chkpt_header.chunk_size;
      ref_id = chkpt_header.ref_id;
      cur_id = chkpt_header.chkpt_id;
//      STDOUT_PRINT("Cur ID: %u\n", cur_id);
//      STDOUT_PRINT("Ref ID: %u\n", ref_id);
//      STDOUT_PRINT("Num distinct: %u\n", chkpt_header.distinct_size);
//      STDOUT_PRINT("Num current: %u\n", chkpt_header.curr_repeat_size);
//      STDOUT_PRINT("Num previous: %u\n", chkpt_header.prev_repeat_size);
      num_distinct = chkpt_header.distinct_size;
//      printf("Setup num distinct: %u\n", num_distinct);

      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+":Setup:Clear distinct/repeat maps");
      distinct_map.clear();
      repeat_map.clear();
      Kokkos::Profiling::popRegion();

      curr_repeat_offset = sizeof(header_t) + chkpt_header.distinct_size*sizeof(uint32_t);
      prev_repeat_offset = chkpt_header.num_prior_chkpts*2*sizeof(uint32_t) + curr_repeat_offset + chkpt_header.curr_repeat_size*2*sizeof(uint32_t);
      data_offset = prev_repeat_offset + chkpt_header.prev_repeat_size*(sizeof(uint32_t)+sizeof(uint32_t));
      curr_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
      prev_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(prev_repeat_offset, data_offset));
      distinct      = Kokkos::subview(chkpt_buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
      data_subview  = Kokkos::subview(chkpt_buffer_d, std::make_pair(data_offset, filesize));
//      STDOUT_PRINT("Checkpoint %u\n", chkpt_header.chkpt_id);
//      STDOUT_PRINT("Checkpoint size: %lu\n", filesize);
//      STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//      STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//      STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//      STDOUT_PRINT("Data offset: %lu\n", data_offset);

      uint32_t num_prior_chkpts = chkpt_header.num_prior_chkpts;
      
      Kokkos::View<uint64_t[1]> counter_d("Write counter");
      auto counter_h = Kokkos::create_mirror_view(counter_d);
      Kokkos::deep_copy(counter_d, 0);
  
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+":Setup:Resize distinct/repeat maps");
      Kokkos::resize(distinct_nodes, num_distinct);
      Kokkos::resize(chunk_len, num_distinct);
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Load maps");
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Load maps:Distinct");
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Load maps:Calculate num distinct chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node;
        memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
        uint32_t len = num_leaf_descendents(node, num_nodes);
        distinct_nodes(i) = node;
        chunk_len(i) = len;
      });
      Kokkos::parallel_scan("Tree:"+std::to_string(idx)+":Load maps:Calc distinct offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
        const uint32_t len = chunk_len(i);
        if(is_final) chunk_len(i) = partial_sum;
        partial_sum += len;
      });
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Load maps:Restart Hashtree distinct", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t node = distinct_nodes(i);
        distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
        uint32_t start = leftmost_leaf(node, num_nodes);
        uint32_t len = num_leaf_descendents(node, num_nodes);
        uint32_t left = 2*node+1;
        uint32_t right = 2*node+2;
        DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu with region offset %u\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size, chunk_len(i));
        while(left < num_nodes) {
          if(right >= num_nodes)
            right = num_nodes;
          for(uint32_t u=left; u<=right; u++) {
            uint32_t leaf = leftmost_leaf(u, num_nodes);
            auto result = distinct_map.insert(NodeID(u, cur_id), chunk_len(i)*chunk_size + (leaf-start)*chunk_size);
            if(result.failed())
              printf("Failed to insert (%u,%u): %u\n", u, cur_id, chunk_len(i)*chunk_size+(leaf-start)*chunk_size);
          }
          left = 2*left+1;
          right = 2*right+2;
        }
      });
  
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Load maps:Repeat");
//printf("Chkpt: %u, num_distinct: %u\n", cur_id, num_distinct);
      Kokkos::View<uint32_t*> repeat_region_sizes("Repeat entires per chkpt", cur_id+1);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Load maps:Load repeat map", Kokkos::RangePolicy<>(0,num_prior_chkpts), KOKKOS_LAMBDA(const uint32_t i) {
        uint32_t chkpt;
        memcpy(&chkpt, chkpt_buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t), sizeof(uint32_t));
        memcpy(&repeat_region_sizes(chkpt), chkpt_buffer_d.data()+curr_repeat_offset+i*2*sizeof(uint32_t)+sizeof(uint32_t), sizeof(uint32_t));
        DEBUG_PRINT("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
//printf("Chkpt: %u, region size: %u\n", chkpt, repeat_region_sizes(chkpt));
      });
      Kokkos::parallel_scan("Tree:"+std::to_string(idx)+":Load maps:Calc repeat offsets", cur_id+1, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
        partial_sum += repeat_region_sizes(i);
        if(is_final) repeat_region_sizes(i) = partial_sum;
      });

      DEBUG_PRINT("Num repeats: %u\n", chkpt_header.curr_repeat_size+chkpt_header.prev_repeat_size);
  
      uint32_t num_repeat_entries = chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size;

      Kokkos::TeamPolicy<> repeat_policy(num_repeat_entries, Kokkos::AUTO);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Load maps:Restart Hashtree repeats", repeat_policy, 
                           KOKKOS_LAMBDA(const Kokkos::TeamPolicy<>::member_type& team_member) {
        uint32_t i = team_member.league_rank();
        uint32_t node, prev, tree=0;
        memcpy(&node, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
        memcpy(&prev, curr_repeat.data()+(num_prior_chkpts)*2*sizeof(uint32_t)+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
        for(uint32_t j=repeat_region_sizes.size()-1; j<repeat_region_sizes.size(); j--) {
          if(i < repeat_region_sizes(j)) {
            tree = j;
          }
        }
        if(team_member.team_rank() == 0) {
          auto result = repeat_map.insert(node, NodeID(prev,tree));
          if(result.failed())
            STDOUT_PRINT("Failed to insert previous repeat %u: (%u,%u) into repeat map\n", node, prev, tree);
        }
        uint32_t curr_start = leftmost_leaf(node, num_nodes);
        uint32_t prev_start = leftmost_leaf(prev, num_nodes);
        uint32_t len = num_leaf_descendents(node, num_nodes);
        Kokkos::parallel_for(Kokkos::TeamThreadRange(team_member, len), [&] (const uint32_t u) {
          repeat_map.insert(curr_start+u, NodeID(prev_start+u, tree));
        });
      });
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(idx)+" Fill chunks");

Kokkos::View<uint32_t[1]> curr_identical_counter("Num identical entries in current checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror curr_identical_counter_h = Kokkos::create_mirror_view(curr_identical_counter);
Kokkos::deep_copy(curr_identical_counter, 0);
Kokkos::View<uint32_t[1]> prev_identical_counter("Num identical entries in previous checkpoint");
Kokkos::View<uint32_t[1]> base_identical_counter("Num identical entries in baseline checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror prev_identical_counter_h = Kokkos::create_mirror_view(prev_identical_counter);;
Kokkos::View<uint32_t[1]>::HostMirror base_identical_counter_h = Kokkos::create_mirror_view(base_identical_counter);;
Kokkos::deep_copy(prev_identical_counter, 0);
Kokkos::deep_copy(base_identical_counter, 0);
Kokkos::deep_copy(total_region_size, 0);
Kokkos::View<uint32_t[1]> curr_chunks("Num identical entries in current checkpoint");
Kokkos::View<uint32_t[1]>::HostMirror curr_chunks_h = Kokkos::create_mirror_view(curr_chunks);
Kokkos::deep_copy(curr_chunks, 0);
//printf("Num chunks: %u\n", num_chunks);
      Kokkos::parallel_for("Tree:"+std::to_string(idx)+":Fill data middle chkpts", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
        if(node_list(i).tree == current_id) {
Kokkos::atomic_add(&curr_chunks(0), 1);
          NodeID id = node_list(i);
          if(distinct_map.exists(id)) {
            uint32_t len = num_leaf_descendents(id.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
            uint32_t start = leftmost_leaf(id.node, num_nodes);
            uint32_t end = rightmost_leaf(id.node, num_nodes);
            size_t offset = distinct_map.value_at(distinct_map.find(id));
            uint32_t writesize = chunk_size;
            if(i*chunk_size+writesize > datalen) 
              writesize = datalen-i*chunk_size;
            memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
            Kokkos::atomic_add(&counter_d(0), writesize);
Kokkos::atomic_add(&curr_identical_counter(0), writesize);
          } else if(repeat_map.exists(id.node)) {
            NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
            DEBUG_PRINT("Repaeat value: %u: (%u,%u)\n", id.node, prev.node, prev.tree);
            if(prev.tree == current_id) {
              if(!repeat_map.exists(id.node))
                printf("Failed to find repeat chunk %u\n", id.node);
              size_t offset = distinct_map.value_at(distinct_map.find(prev));
              uint32_t len = num_leaf_descendents(prev.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
              uint32_t start = leftmost_leaf(prev.node, num_nodes);
              uint32_t writesize = chunk_size;
              if(i*chunk_size+writesize > datalen) 
                writesize = datalen-i*chunk_size;
              memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
              Kokkos::atomic_add(&counter_d(0), writesize);
Kokkos::atomic_add(&curr_identical_counter(0), writesize);
            } else {
              node_list(i) = prev;
//if(prev.tree == 0)
//Kokkos::atomic_add(&base_identical_counter(0), 1);
//Kokkos::atomic_add(&prev_identical_counter(0), 1);
            }
          } else {
Kokkos::atomic_add(&prev_identical_counter(0), 1);
            node_list(i) = NodeID(node_list(i).node, current_id-1);
          }
//Kokkos::atomic_add(&curr_identical_counter(0), 1);
        } else if(node_list(i).tree < current_id) {
Kokkos::atomic_add(&base_identical_counter(0), 1);
//        } else if(node_list(i).tree < current_id) {
//Kokkos::atomic_add(&prev_identical_counter(0), 1);
        }
      });
      Kokkos::Profiling::popRegion();
      Kokkos::Profiling::popRegion();
//Kokkos::deep_copy(total_region_size_h, total_region_size);
//printf("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));
//Kokkos::deep_copy(base_identical_counter_h, base_identical_counter);
//Kokkos::deep_copy(prev_identical_counter_h, prev_identical_counter);
//Kokkos::deep_copy(curr_identical_counter_h, curr_identical_counter);
//Kokkos::deep_copy(curr_chunks_h, curr_chunks);
//printf("Number of chunks to test: %u\n", curr_chunks_h(0));
//printf("Number of bytes written for chkpt %u: %u\n", current_id, curr_identical_counter_h(0));
//printf("Number of identical chunks for chkps prior to %u: %u\n", current_id, prev_identical_counter_h(0));
//printf("Number of chunks for prior checkpoints: %u\n", base_identical_counter_h(0));
    }

    // Reference
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id));
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+":Read checkpoint");
    t1 = std::chrono::high_resolution_clock::now();
//    file.open(chkpt_files[header.ref_id], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
//    size_t chkpt_size = file.tellg();
//    file.seekg(0);
    auto& chkpt_buffer_d = buffers_d[header.ref_id];
    auto& chkpt_buffer_h = buffers_h[header.ref_id];
    size_t chkpt_size = chkpt_buffer_d.size();
    Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
//    auto chkpt_buffer_d = buffer_d;
//    auto chkpt_buffer_h = buffer_h;
//    Kokkos::resize(chkpt_buffer_d, chkpt_size);
//    Kokkos::resize(chkpt_buffer_h, chkpt_size);
//    Kokkos::View<uint8_t*> chkpt_buffer_d("Checkpoint buffer", chkpt_size);
//    auto chkpt_buffer_h = Kokkos::create_mirror_view(chkpt_buffer_d);
//    file.read((char*)(chkpt_buffer_h.data()), chkpt_size);
//    file.close();
    t2 = std::chrono::high_resolution_clock::now();
    STDOUT_PRINT("Time spent reading checkpoint %u from file: %f\n", header.ref_id, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
//    printf("Time spent reading checkpoint %u from file: %f\n", header.ref_id, (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(t2-t1).count()));
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Setup");
    header_t chkpt_header;
    memcpy(&chkpt_header, chkpt_buffer_h.data(), sizeof(header_t));
    uint32_t current_id = chkpt_header.chkpt_id;
    datalen = chkpt_header.datalen;
    chunk_size = chkpt_header.chunk_size;
//    Kokkos::deep_copy(chkpt_buffer_d, chkpt_buffer_h);
    ref_id = chkpt_header.ref_id;
    cur_id = chkpt_header.chkpt_id;

    curr_repeat_offset = sizeof(header_t) + chkpt_header.distinct_size*sizeof(uint32_t);
    prev_repeat_offset = curr_repeat_offset + chkpt_header.curr_repeat_size*2*sizeof(uint32_t);
    data_offset = prev_repeat_offset + chkpt_header.prev_repeat_size*2*sizeof(uint32_t);
    curr_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(curr_repeat_offset, prev_repeat_offset));
    prev_repeat   = Kokkos::subview(chkpt_buffer_d, std::make_pair(prev_repeat_offset, data_offset));
    distinct      = Kokkos::subview(chkpt_buffer_d, std::make_pair(sizeof(header_t), curr_repeat_offset));
    data_subview = Kokkos::subview(chkpt_buffer_d, std::make_pair(data_offset, chkpt_size));
//    STDOUT_PRINT("Checkpoint %u\n", chkpt_header.chkpt_id);
//    STDOUT_PRINT("Checkpoint size: %lu\n", chkpt_size);
//    STDOUT_PRINT("Distinct offset: %lu\n", sizeof(header_t));
//    STDOUT_PRINT("Curr repeat offset: %lu\n", curr_repeat_offset);
//    STDOUT_PRINT("Prev repeat offset: %lu\n", prev_repeat_offset);
//    STDOUT_PRINT("Data offset: %lu\n", data_offset);
    
    distinct_map.clear();
    distinct_map.rehash(chkpt_header.distinct_size);
    Kokkos::fence();
    repeat_map.clear();
//    Kokkos::UnorderedMap<uint32_t, NodeID> repeat_map(chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size);
    num_distinct = chkpt_header.distinct_size;
    Kokkos::resize(distinct_nodes, num_distinct);
    Kokkos::resize(chunk_len, num_distinct);
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Load maps");
    Kokkos::parallel_for("Tree:Reference:Calculate num chunks", Kokkos::RangePolicy<>(0, num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      memcpy(&node, distinct.data()+i*sizeof(uint32_t), sizeof(uint32_t));
      uint32_t len = num_leaf_descendents(node, num_nodes);
      distinct_nodes(i) = node;
      chunk_len(i) = len;
    });
    Kokkos::parallel_scan("Tree:Reference:Calc offsets", num_distinct, KOKKOS_LAMBDA(const uint32_t i, uint32_t& partial_sum, bool is_final) {
      const uint32_t len = chunk_len(i);
      if(is_final) chunk_len(i) = partial_sum;
      partial_sum += len;
    });

    Kokkos::parallel_for("Tree:Reference:Fill distinct map", Kokkos::RangePolicy<>(0,num_distinct), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node = distinct_nodes(i);
      distinct_map.insert(NodeID(node, cur_id), chunk_len(i)*chunk_size);
      DEBUG_PRINT("Reading region: (%u,%u) at %lu: %lu\n", node, cur_id, i*sizeof(uint32_t), data_offset+chunk_len(i)*chunk_size);
    });

    uint32_t num_repeat = chkpt_header.curr_repeat_size + chkpt_header.prev_repeat_size;
    Kokkos::parallel_for("Tree:Reference:Fill repeat map", Kokkos::RangePolicy<>(0, num_repeat), KOKKOS_LAMBDA(const uint32_t i) {
      uint32_t node;
      uint32_t prev;
      memcpy(&node, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t)), sizeof(uint32_t));
      memcpy(&prev, curr_repeat.data()+i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t), sizeof(uint32_t));
      auto result = repeat_map.insert(node, NodeID(prev, ref_id));
      if(result.failed())
        STDOUT_PRINT("Reference: Failed to insert repeat node %u: (%u,%u)\n", node, prev, ref_id);
      DEBUG_PRINT("Reference: Inserted repeat node %u: (%u,%u): %lu\n", node, prev, ref_id, i*(sizeof(uint32_t)+sizeof(uint32_t))+sizeof(uint32_t));
    });
Kokkos::deep_copy(total_region_size, 0);
Kokkos::Profiling::popRegion();
Kokkos::Profiling::pushRegion("Checkpoint "+std::to_string(header.ref_id)+" Fill chunks");
    Kokkos::parallel_for("Tree:Reference:Fill data reference chkpt", Kokkos::RangePolicy<>(0, num_chunks), KOKKOS_LAMBDA(const uint32_t i) {
      if(node_list(i).tree == current_id) {
        NodeID id = node_list(i);
        if(distinct_map.exists(id)) {
          uint32_t len = num_leaf_descendents(id.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
          uint32_t start = leftmost_leaf(id.node, num_nodes);
          size_t offset = distinct_map.value_at(distinct_map.find(id));
          uint32_t writesize = chunk_size*len;
          Kokkos::atomic_add(&counter_d(0), writesize);
          if((start+len-1-num_chunks+1)*chunk_size >= datalen)
            writesize = datalen-(start-num_chunks+1)*chunk_size;
          memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize);
        } else if(repeat_map.exists(id.node)) {
          NodeID prev = repeat_map.value_at(repeat_map.find(id.node));
          if(prev.tree == current_id) {
            size_t offset = distinct_map.value_at(distinct_map.find(prev));
            
            uint32_t len = num_leaf_descendents(prev.node, num_nodes);
Kokkos::atomic_add(&total_region_size(0), len);
            uint32_t start = leftmost_leaf(prev.node, num_nodes);
            uint32_t writesize = chunk_size*len;
            Kokkos::atomic_add(&counter_d(0), writesize);
            if((start+len-1-num_chunks+1)*chunk_size >= datalen)
              writesize = datalen-(start-num_chunks+1)*chunk_size;

            memcpy(data.data()+chunk_size*i, data_subview.data()+offset, writesize); 
          } else {
            node_list(i) = prev;
          }
        } else {
          node_list(i) = NodeID(node_list(i).node, current_id-1);
        }
      }
    });
//Kokkos::deep_copy(total_region_size_h, total_region_size);
//printf("Chkpt %u: total region size: %u\n", cur_id, total_region_size_h(0));
//printf("Chkpt %u: avg region size: %u\n", cur_id, total_region_size_h(0));
    Kokkos::fence();
Kokkos::Profiling::popRegion();
Kokkos::Profiling::popRegion();
    std::chrono::high_resolution_clock::time_point c3 = std::chrono::high_resolution_clock::now();
    double copy_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c2-c1).count());
    double restart_time = (1e-9)*(std::chrono::duration_cast<std::chrono::nanoseconds>(c3-t0).count());
    return std::make_pair(copy_time, restart_time);
}

std::pair<double,double> 
restart_incr_chkpt_hashtree(std::vector<std::string>& chkpt_files,
                            const int file_idx, 
                            Kokkos::View<uint8_t*>& data) {
  // Read main incremental checkpoint header
  std::ifstream file;
  file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
//size_t max_size = 0;
//for(uint32_t i=0; i<chkpt_files.size(); i++) {
//std::cout << "Opening file: " << chkpt_files[i] << std::endl;
//  file.open(chkpt_files[i], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
//  size_t filesize = file.tellg();
//  if(filesize > max_size)
//    max_size = filesize;
//  file.close();
//printf("File %u with size %lu\n", i, filesize);
//}
//printf("Max size: %lu\n", max_size);
  file.open(chkpt_files[file_idx], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
  size_t filesize = file.tellg();
  file.seekg(0);

//  DEBUG_PRINT("File size: %zd\n", filesize);
  header_t header;
  file.read((char*)&header, sizeof(header_t));
  file.close();
  STDOUT_PRINT("Ref ID: %u\n",               header.ref_id);
  STDOUT_PRINT("Chkpt ID: %u\n",             header.chkpt_id);
  STDOUT_PRINT("Data len: %lu\n",            header.datalen);
  STDOUT_PRINT("Chunk size: %u\n",           header.chunk_size);
  STDOUT_PRINT("Window size: %u\n",          header.window_size);
  STDOUT_PRINT("Distinct size: %u\n",        header.distinct_size);
  STDOUT_PRINT("Current Repeat size: %u\n",  header.curr_repeat_size);
  STDOUT_PRINT("Previous Repeat size: %u\n", header.prev_repeat_size);

  std::vector<Kokkos::View<uint8_t*>> chkpts_d;
  std::vector<Kokkos::View<uint8_t*>::HostMirror> chkpts_h;
  for(uint32_t i=0; i<chkpt_files.size(); i++) {
//printf("Trying to read checkpint %u\n", i);
    file.open(chkpt_files[i], std::ifstream::in | std::ifstream::binary | std::ifstream::ate);
    size_t filesize = file.tellg();
//printf("Checkpoint %u is %lu bytes\n", i, filesize);
    file.seekg(0);
    Kokkos::View<uint8_t*> chkpt_d("Checkpoint", filesize);
    auto chkpt_h = Kokkos::create_mirror_view(chkpt_d);;
//printf("Allocated Views\n");
    file.read((char*)(chkpt_h.data()), filesize);
    file.close();
//printf("Read checkpoint to memory\n");
    chkpts_d.push_back(chkpt_d);
    chkpts_h.push_back(chkpt_h);
//printf("Loaded checkpoint %u\n", i);
  }
//printf("Done loading checkpoints\n");

  Kokkos::View<uint8_t*> buffer_d("Buffer", filesize);
//  Kokkos::View<uint8_t*> buffer_d("Buffer", 1);
  Kokkos::deep_copy(buffer_d, 0);
  auto buffer_h = Kokkos::create_mirror_view(buffer_d);
  Kokkos::deep_copy(buffer_h, 0);
//  file.close();

  uint32_t num_chunks = header.datalen / header.chunk_size;
  if(num_chunks*header.chunk_size < header.datalen) {
    num_chunks += 1;
  }
  uint32_t num_nodes = 2*num_chunks-1;
  Kokkos::resize(data, header.datalen);

  std::pair<double,double> times;

//printf("Run restart functions\n");

  // Only deduplicated with baseline checkpoint
  if(header.window_size == 0) {
    times = restart_chkpt_local(chkpt_files, file_idx, file, data, filesize, num_chunks, num_nodes, header, buffer_d, buffer_h);
  } else { // Checkpoint uses full history of checkpoints for deduplication
//    times = restart_chkpt_global(chkpt_files, file_idx, file, data, filesize, num_chunks, num_nodes, header, buffer_d, buffer_h);
    times = restart_chkpt_global(chkpt_files, file_idx, chkpts_d, chkpts_h, data, filesize, num_chunks, num_nodes, header);
  }
  Kokkos::fence();
  DEBUG_PRINT("Restarted checkpoint\n");
  return times;
}

#endif // RESTART_MERKLE_TREE_HPP
