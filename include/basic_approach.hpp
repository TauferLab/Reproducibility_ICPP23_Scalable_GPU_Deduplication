#ifndef BASIC_APPROACH_HPP
#define BASIC_APPROACH_HPP

#include <Kokkos_Core.hpp>
#include <Kokkos_UnorderedMap.hpp>
#include <Kokkos_ScatterView.hpp>
#include <Kokkos_Sort.hpp>
#include <Kokkos_Bitset.hpp>
#include <climits>
#include <chrono>
#include <fstream>
#include <vector>
#include <utility>
#include "kokkos_hash_list.hpp"
#include "hash_functions.hpp"
#include "map_helpers.hpp"
#include "utils.hpp"
#include "deduplicator_interface.hpp"

class BasicDeduplicator : public BaseDeduplicator {
  private:
    HashList list;
    Kokkos::Bitset<Kokkos::DefaultExecutionSpace> changes_bitset;
    uint32_t num_chunks;

    void dedup_data(const uint8_t* data_ptr, 
                    const size_t len);

    std::pair<uint64_t,uint64_t> 
    collect_diff( const uint8_t* data_ptr, 
                  const size_t len,
                  Kokkos::View<uint8_t*>& buffer_d, 
                  header_t& header);

    std::pair<double,double>
    restart_chkpt( std::vector<Kokkos::View<uint8_t*>::HostMirror>& incr_chkpts,
                   const int chkpt_idx, 
                   Kokkos::View<uint8_t*>& data);

    std::pair<double,double>
    restart_chkpt( std::vector<std::string>& chkpt_files,
                         const int file_idx, 
                         Kokkos::View<uint8_t*>& data);
  public:
    BasicDeduplicator();

    BasicDeduplicator(uint32_t bytes_per_chunk);

    ~BasicDeduplicator() override;

    /**
     * Main checkpointing function. Given a Kokkos View, create an incremental checkpoint using 
     * the chosen checkpoint strategy. The deduplication mode can be one of the following:
     *   - Full: No deduplication
     *   - Basic: Remove chunks that have not changed since the previous checkpoint
     *   - List: Save a single copy of each unique chunk and use metadata to handle duplicates
     *   - Tree: Save minimal set of chunks and use a compact metadata representations
     *
     * \param header        The checkpoint header
     * \param data_ptr      Data to be checkpointed
     * \param data_len      Length of data in bytes
     * \param diff_h        The output incremental checkpoint on the Host
     * \param make_baseline Flag determining whether to make a baseline checkpoint
     */
    void checkpoint(header_t& header, 
                    uint8_t* data_ptr, 
                    size_t data_len,
                    Kokkos::View<uint8_t*>::HostMirror& diff_h, 
                    bool make_baseline) override ;

    /**
     * Main checkpointing function. Given a raw device pointer, create an incremental checkpoint 
     * using the chosen checkpoint strategy. The deduplication mode can be one of the following:
     *   - Full: No deduplication
     *   - Basic: Remove chunks that have not changed since the previous checkpoint
     *   - List: Save a single copy of each unique chunk and use metadata to handle duplicates
     *   - Tree: Save minimal set of chunks and use a compact metadata representations
     *
     * \param data_ptr      Raw data pointer that needs to be deduplicated
     * \param len           Length of data
     * \param filename      Filename to save checkpoint
     * \param logname       Base filename for logs
     * \param make_baseline Flag determining whether to make a baseline checkpoint
     */
    void checkpoint(uint8_t* data_ptr, 
                    size_t len, 
                    std::string& filename, 
                    std::string& logname, 
                    bool make_baseline) override;

    /**
     * Main checkpointing function. Given a raw device pointer, create an incremental checkpoint 
     * using the chosen checkpoint strategy. Save checkpoint to host view. 
     * The deduplication mode can be one of the following:
     *   - Full: No deduplication
     *   - Basic: Remove chunks that have not changed since the previous checkpoint
     *   - List: Save a single copy of each unique chunk and use metadata to handle duplicates
     *   - Tree: Save minimal set of chunks and use a compact metadata representations
     *
     * \param data_ptr      Raw data pointer that needs to be deduplicated
     * \param len           Length of data
     * \param diff_h        Host View to store incremental checkpoint
     * \param make_baseline Flag determining whether to make a baseline checkpoint
     */
    void checkpoint(uint8_t* data_ptr, 
                    size_t len, 
                    Kokkos::View<uint8_t*>::HostMirror& diff_h, 
                    bool make_baseline) override;

    /**
     * Main checkpointing function. Given a raw device pointer, create an incremental checkpoint 
     * using the chosen checkpoint strategy. Save checkpoint to host view and write logs.
     * The deduplication mode can be one of the following:
     *   - Full: No deduplication
     *   - Basic: Remove chunks that have not changed since the previous checkpoint
     *   - List: Save a single copy of each unique chunk and use metadata to handle duplicates
     *   - Tree: Save minimal set of chunks and use a compact metadata representations
     *
     * \param data_ptr      Raw data pointer that needs to be deduplicated
     * \param len           Length of data
     * \param diff_h        Host View to store incremental checkpoint
     * \param logname       Base filename for logs
     * \param make_baseline Flag determining whether to make a baseline checkpoint
     */
    void checkpoint(uint8_t* data_ptr, 
                    size_t len, 
                    Kokkos::View<uint8_t*>::HostMirror& diff_h, 
                    std::string& logname, 
                    bool make_baseline) override;

    /**
     * Restart checkpoint from vector of incremental checkpoints loaded on the Host.
     *
     * \param data       Data View to restart checkpoint into
     * \param chkpts     Vector of prior incremental checkpoints stored on the Host
     * \param logname    Filename for restart logs
     * \param chkpt_id   ID of checkpoint to restart
     */
    void restart(Kokkos::View<uint8_t*> data, 
                 std::vector<Kokkos::View<uint8_t*>::HostMirror>& chkpts, 
                 std::string& logname, 
                 uint32_t chkpt_id) override;

    /**
     * Restart checkpoint from vector of incremental checkpoints loaded on the Host. 
     * Store result into raw device pointer.
     *
     * \param data_ptr   Device pointer to save checkpoint in
     * \param len        Length of data
     * \param chkpts     Vector of prior incremental checkpoints stored on the Host
     * \param logname    Filename for restart logs
     * \param chkpt_id   ID of checkpoint to restart
     */
    void restart(uint8_t* data_ptr, 
                 size_t len, 
                 std::vector<Kokkos::View<uint8_t*>::HostMirror>& chkpts, 
                 std::string& logname, 
                 uint32_t chkpt_id) override;

    /**
     * Restart checkpoint from checkpoint files
     *
     * \param data       Data View to restart checkpoint into
     * \param filenames  Vector of prior incremental checkpoints stored in files
     * \param logname    Filename for restart logs
     * \param chkpt_id   ID of checkpoint to restart
     */
    void restart(Kokkos::View<uint8_t*> data, 
                 std::vector<std::string>& chkpt_filenames, 
                 std::string& logname, 
                 uint32_t chkpt_id) override;

    /**
     * Write logs for the checkpoint metadata/data breakdown, runtimes, and the overall summary.
     * The data breakdown log shows the proportion of data and metadata as well as how much 
     * metadata corresponds to each prior checkpoint.
     * The timing log contains the time spent comparing chunks, gathering scattered chunks,
     * and the time spent copying the resulting checkpoint from the device to host.
     *
     * \param header  The checkpoint header
     * \param diff_h  The incremental checkpoint
     * \param logname Base filename for the logs
     */
    void write_chkpt_log(header_t& header, 
                         Kokkos::View<uint8_t*>::HostMirror& diff_h, 
                         std::string& logname) override;
    /**
     * Function for writing the restart log.
     *
     * \param select_chkpt Which checkpoint to write the log
     * \param logname      Filename for writing log
     */
    void write_restart_log(uint32_t select_chkpt, 
                           std::string& logname) override;
};

/**
 * Deduplicate provided data view using the basic incremental checkpoint approach. 
 * Split data into chunks and compute hashes for each chunk. Compare each hash with 
 * the hash at the same offset. If the hash is different then we save the chunk in the diff.
 *
 * \brief list       List of hashes for identifying differences
 * \brief changes    Bitset for marking which chunks have changed since the prior checkpoint
 * \brief list_id    ID for the current checkpoint
 * \brief data       View of data for deduplicating
 * \brief chunk_size Size in bytes for splitting data into chunks
 */
void dedup_data_basic(  
                  const HashList& list, 
                  Kokkos::Bitset<Kokkos::DefaultExecutionSpace>& changes,
                  const uint32_t list_id, 
                  const uint8_t* data_ptr, 
                  const size_t data_len,
                  const uint32_t chunk_size);

/**
 * Gather the scattered chunks for the diff and write the checkpoint to a contiguous buffer.
 *
 * \param data Data View containing data to deduplicate
 * \param buffer_d       View to store the diff in
 * \param chunk_size     Size of chunks in bytes
 * \param changes        Bitset identifying which chunks have changed
 * \param prior_chkpt_id ID of the last checkpoint
 * \param chkpt_id       ID for the current checkpoint
 * \param header         Incremental checkpoint header
 *
 * \return Pair containing amount of data and metadata in the checkpoint
 */
std::pair<uint64_t,uint64_t> 
write_diff_basic( const uint8_t* data_ptr, 
                  const size_t data_len,
                  Kokkos::View<uint8_t*>& buffer_d, 
                  uint32_t chunk_size, 
                  Kokkos::Bitset<Kokkos::DefaultExecutionSpace>& changes,
                  uint32_t prior_chkpt_id,
                  uint32_t chkpt_id,
                  header_t& header);

/**
 * Restart data from incremental checkpoint.
 *
 * \param incr_chkpts Vector of Host Views containing the diffs
 * \param chkpt_idx   ID of which checkpoint to restart
 * \param data        View for restarting the checkpoint to
 *
 * \return Time spent copying incremental checkpoints from host to device and restarting data
 */
std::pair<double,double>
restart_chkpt_basic( std::vector<Kokkos::View<uint8_t*>::HostMirror>& incr_chkpts,
                     const int chkpt_idx, 
                     Kokkos::View<uint8_t*>& data);

std::pair<double,double>
restart_chkpt_basic( std::vector<std::string>& chkpt_files,
                     const int file_idx, 
                     Kokkos::View<uint8_t*>& data);

#endif // BASIC_APPROACH_HPP
