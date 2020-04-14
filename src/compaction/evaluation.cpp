#include "params.h"
#ifdef COMSYS_COMPACTION

#include "evaluation.h"
#include <random.h>
#include <amount.h>
#include <script/standard.h>
#include <pubkey.h>
#include <logging.h>
#include <shutdown.h>

#ifdef ENABLE_COMPACTION
#ifdef ENABLE_EVALUATION

#include <fs.h>
#include <validation.h>
#include <init.h>
#include <iomanip>
#include <sys/stat.h>

const int64_t US_SCALE_FACTOR = 1000; // us to ms scaling (uses int64_t!)

const std::vector<EvalTimerNames> all_eval_timers = {
    EVAL_TIMER_SYNC_TOTAL,
    EVAL_TIMER_PREPARATION_TOTAL,
    EVAL_TIMER_FULL_SYNC_TOTAL,
    EVAL_TIMER_BLOCK_TOTAL,
    EVAL_TIMER_CHECK_BLOCK,
    EVAL_TIMER_STORE_BLOCK,
    EVAL_TIMER_CONNECT_BLOCK,
};

/* Evaluation perspectives */
/* For convenience in the code, all perspectives are allocated, but they have to be
 * explicitly activated. This allows interleaving evaluation methods and enables
 * better handling of them.
 */
std::unique_ptr<EvalHeaderchainDownload> evalp_headerchain_download = nullptr;
std::unique_ptr<EvalVanillaSynchronization> evalp_vanilla_synchronization = nullptr;
std::unique_ptr<EvalStateCreation> evalp_state_creation = nullptr;
std::unique_ptr<EvalCompactionSynchronization> evalp_compaction_synchronization = nullptr;
std::unique_ptr<EvalSavingPotential> evalp_saving_potential = nullptr;

unsigned int eval_start_state_height = 0;
unsigned int eval_last_state_height = 0;
unsigned int eval_state_height = 0;
unsigned int eval_tail_length = 1010;
unsigned int shutdownAtHeight = 0;
unsigned int eval_number_outbound_peers = 0;

std::unique_ptr<CompactionState> dummy_compaction_state = nullptr;

const std::string GetEvalDir() {
    return gArgs.GetArg("-evaldir", GetDefaultEvalDir()); // We need to check during initialization
}

const std::string GetDefaultEvalDir() {
    return "/tmp/bitcoin-compaction/eval";
}


/*****************************************************************************************
 *
 * Eval Perspectives
 *
 ****************************************************************************************/

/* An evaluation perspective is a bundle of functions to be associated with a single
 * aspect of evaluation. It's a wrapper for a single evaluation file that may have
 * individual rules (custom header, stateless/stateful, etc.)
 */


bool EvalPerspective::createFile() {
    char tmp[128];
    gethostname(tmp, 128);
    std::string filename = GetEvalDir() + "/" + this->method_name + "_" + tmp + ".csv";
    struct stat buf;
    bool already_existed = (stat(filename.c_str(), &buf) != -1);

    if (this->output_file.is_open()) {
        this->output_file.close();
    }
    this->output_file.open(filename, std::ios_base::app);
    return already_existed;
}

void EvalPerspective::writeData(std::string data) {
    this->output_file << data << std::endl;
}

const std::string EvalPerspective::getMethodName() {
    return this->method_name;
}

bool EvalPerspective::isActive() {
    return this->active;
}

void EvalPerspective::close() {
    if (this->output_file.is_open()) {
        this->output_file.close();
    }
}



/*****************************************************************************************
 *
 * Eval Perspective: Vanilla Synchronization
 *
 ****************************************************************************************/

void EvalVanillaSynchronization::writeHeader() {
    std::stringstream measurement_header;
    measurement_header
        << "n_run" << ","
        << "n_block_height" << ","
        << "b_tail_phase" << ","
        << "t_sync_total_ms" << ","
        << "t_preparation_ms" << ","
        << "n_blocks_processed_total" << ","
        << "t_check_blocks_total_ms" << ","
        << "t_store_blocks_total_ms" << ","
        << "t_apply_block_to_utxo_ms" << ","
        << "t_processing_total_ms" << ","
        << "t_full_sync_total_ms" << ","
        << "n_total_bytes_sent" << ","
        << "n_total_bytes_recv";
    this->writeData(measurement_header.str());
}

void EvalVanillaSynchronization::startMeasurement(EvalTimerNames timer_name) {
    this->timers[timer_name].reset();
    this->timers[timer_name].start();
}

void EvalVanillaSynchronization::stopMeasurement(EvalTimerNames timer_name) {
    this->timers[timer_name].stop();
}

unsigned int EvalVanillaSynchronization::getBlockcount() {
    return this->n_block_count;
}

void EvalVanillaSynchronization::incrementBlockcount() {
    this->n_block_count += 1;
    if (this->n_block_count > this->n_state_height) {
        this->enterTailPhase();
    }
}

void EvalVanillaSynchronization::doMeasurement(unsigned int block_height) {
    std::stringstream result;
    result
        << this->n_run << ","
        << block_height << ","
        << this->inTailPhase() << ","
        << this->timers[EVAL_TIMER_SYNC_TOTAL].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_PREPARATION_TOTAL].getCumulativeDuration() << ","
        << this->n_block_count << ","
        << this->timers[EVAL_TIMER_CHECK_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_STORE_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_CONNECT_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_BLOCK_TOTAL].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_FULL_SYNC_TOTAL].getCumulativeDuration() << ","
        << g_connman->GetTotalBytesSent() << ","
        << g_connman->GetTotalBytesRecv();
    this->writeData(result.str());

}

bool EvalVanillaSynchronization::synchronizationFinished() {
    return this->n_block_count >= this->n_block_height_max;
}

void EvalVanillaSynchronization::enterTailPhase() {
    this->in_tail_phase = true;
}

bool EvalVanillaSynchronization::inTailPhase() {
    return this->in_tail_phase && (this->n_block_count <= this->n_block_height_max);
}

unsigned int EvalVanillaSynchronization::getTailLength() {
    return this->n_tail_length;
}

unsigned int EvalVanillaSynchronization::getTargetChainLength() {
    return this->n_state_height + this->n_tail_length;
}

bool EvalVanillaSynchronization::onRelevantTailHeight() {
    if (this->n_block_count < COMPACTION_STEPSIZE) {
        return false;
    }

    unsigned int tail_modulus = this->n_block_count % COMPACTION_STEPSIZE;

    for (unsigned int tail_length : this->tail_lengths) {
        if (tail_modulus == tail_length + 1) {
            return true;
        }
    }
    return false;
}



/*****************************************************************************************
 *
 * Eval Perspective: Compaction Synchronization
 *
 ****************************************************************************************/

void EvalCompactionSynchronization::writeHeader() {
    std::stringstream measurement_header;
    measurement_header
        << "n_run" << ","
        << "n_block_height" << ","
        << "b_tail_phase" << ","
        << "t_sync_total_ms" << ","
        << "t_preparation_ms" << ","
        << "n_blocks_processed_total" << ","
        << "t_check_blocks_total_ms" << ","
        << "t_store_blocks_total_ms" << ","
        << "t_apply_state_to_utxo_ms" << ","
        << "t_processing_total_ms" << ","
        << "t_full_sync_total_ms" << ","
        << "n_total_bytes_sent" << ","
        << "n_total_bytes_recv";
    this->writeData(measurement_header.str());
}

void EvalCompactionSynchronization::startMeasurement(EvalTimerNames timer_name) {
    this->timers[timer_name].reset();
    this->timers[timer_name].start();
}


void EvalCompactionSynchronization::stopMeasurement(EvalTimerNames timer_name) {
    this->timers[timer_name].stop();
}

unsigned int EvalCompactionSynchronization::getBlockcount() {
    return this->n_block_count;
}

void EvalCompactionSynchronization::setBlockcount(unsigned int block_height) {
    if (this->n_block_count != 0) {
        return;
    }
    this->n_block_count = block_height;
}

void EvalCompactionSynchronization::incrementBlockcount() {
    this->n_block_count += 1;
}

void EvalCompactionSynchronization::doMeasurement(unsigned int block_height) {
    std::stringstream result;
    result
        << this->n_run << ","
        << block_height << ","
        << this->inTailPhase() << ","
        << this->timers[EVAL_TIMER_SYNC_TOTAL].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_PREPARATION_TOTAL].getCumulativeDuration() << ","
        << this->n_block_count << ","
        << this->timers[EVAL_TIMER_CHECK_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_STORE_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_CONNECT_BLOCK].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_BLOCK_TOTAL].getCumulativeDuration() << ","
        << this->timers[EVAL_TIMER_FULL_SYNC_TOTAL].getCumulativeDuration() << ","
        << g_connman->GetTotalBytesSent() << ","
        << g_connman->GetTotalBytesRecv();
    this->writeData(result.str());
}

bool EvalCompactionSynchronization::synchronizationFinished() {
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Checking for finished synchronization: %d / %d blocks\n", __FILE__, __func__, __LINE__, this->n_block_count, (this->n_state_height + n_tail_length));
    return this->n_block_count >= (this->n_state_height + n_tail_length);
}

void EvalCompactionSynchronization::enterTailPhase() {
    this->in_tail_phase = true;
}

bool EvalCompactionSynchronization::inTailPhase() {
    return this->in_tail_phase && (this->n_block_count <= this->n_block_height_max);
}

unsigned int EvalCompactionSynchronization::getTailLength() {
    return this->n_tail_length;
}

unsigned int EvalCompactionSynchronization::getTargetChainLength() {
    return this->n_state_height + this->n_tail_length;
}



/*****************************************************************************************
 *
 * Eval Perspective: Headerchain Download
 *
 ****************************************************************************************/

void EvalHeaderchainDownload::writeHeader() {
    std::stringstream measurement_header;
    measurement_header
        << "n_run" << ","
        << "n_block_height" << ","
        << "t_duration_ms";
    this->writeData(measurement_header.str());
}

bool EvalHeaderchainDownload::headerAlreadySeen(unsigned int block_height) {
    return (this->headersReceived.find(block_height) == this->headersReceived.end());
}

void EvalHeaderchainDownload::headerReceived(unsigned int block_height) {
    this->headersReceived[block_height] = true;
}


void EvalHeaderchainDownload::startMeasurement() {
    this->timer.reset();
    this->timer.start();
}

void EvalHeaderchainDownload::stopMeasurement() {
    this->timer.stop();
}

void EvalHeaderchainDownload::doMeasurement(unsigned int block_height) {
    std::stringstream result;
    result
        << this->n_run << ","
        << block_height << ","
        << this->timer.getCumulativeDuration();
    this->writeData(result.str());
}

unsigned int EvalHeaderchainDownload::getBlockcount() {
    return this->n_block_count;
}

void EvalHeaderchainDownload::incrementBlockcount() {
    this->n_block_count += 1;
}

bool EvalHeaderchainDownload::synchronizationFinished() {
    return this->n_block_count >= this->n_block_height_max;
}




/*****************************************************************************************
 *
 * Eval Perspective: State Creation Duration
 *
 ****************************************************************************************/

void EvalStateCreation::writeHeader() {
    std::stringstream measurement_header;
    measurement_header
        << "n_run" << ","
        << "n_blockheight" << ","
        << "t_rewind_and_create_ms" << ","
        << "t_create_only_ms";
    this->writeData(measurement_header.str());
}

void EvalStateCreation::startTimerAll(void) {
    this->timer_all.start();
}

void EvalStateCreation::stopTimerAll(void) {
    this->timer_all.stop();
}

void EvalStateCreation::startTimerCreateOnly(void) {
    this->timer_create.start();
}

void EvalStateCreation::stopTimerCreateOnly(void) {
    this->timer_create.stop();
}

void EvalStateCreation::doMeasurement(unsigned int n_run, unsigned int block_height) {
    std::stringstream result;
    result << n_run << ","
        << block_height << ","
        << this->timer_all.getDuration() << ","
        << this->timer_create.getDuration();

    this->writeData(result.str());
    this->timer_all.reset();
    this->timer_create.reset();
}

void EvalStateCreation::doMeasurement(unsigned int block_height) {

    unsigned int n_run = gArgs.GetArg("-run", 0);

    std::stringstream result;
    result << n_run << ","
        << block_height << ","
        << this->timer_all.getDuration() << ","
        << this->timer_create.getDuration();

    this->writeData(result.str());
    this->timer_all.reset();
    this->timer_create.reset();
}

void EvalStateCreation::doMeasurements(unsigned int block_height) {
    for (unsigned int n_run = 0; n_run < this->n_runs; ++n_run) {
        this->startTimerAll();
        std::unique_ptr<CompactionState> state(CompactionState::createState(block_height));
        this->stopTimerAll();
        state.reset();
        this->doMeasurement(n_run, block_height);
    }
}

void EvalStateCreation::prepareMeasurement(unsigned int block_height) {
    this->stored_block_height = block_height;
}

void EvalStateCreation::doMeasurement(void) {
    this->doMeasurement(this->stored_block_height);
    this->stored_block_height = 0;
}


/*****************************************************************************************
 *
 * Eval Perspective: Saving Potential
 *
 ****************************************************************************************/

void EvalSavingPotential::writeHeader() {
    std::stringstream measurement_header;
    measurement_header
        << "n_run" << ","
        << "n_block_height" << ","
        << "t_block" << ","
        << "size_blocks_path" << ","
        << "size_state_disk" << ","
        << "size_chainstate_path" << ","
        << "size_blockchain_rpc" << ","
        << "cumulative_block_sizes_no_witnesses" << ","
        << "cumulative_block_sizes" << ","
        << "cumulative_block_sizes_since_state_no_witnesses" << ","
        << "cumulative_block_sizes_since_state";
    this->writeData(measurement_header.str());
}

/**************************************
 * Get size of a single file
 **************************************/
uintmax_t EvalSavingPotential::getFileSize(boost::filesystem::path path) {
    if (!boost::filesystem::is_regular_file(path)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: %s is not a regular file.\n", __FILE__, __func__, __LINE__, path.string());
    }
    return boost::filesystem::file_size(path); // file_size() deals with error handling (returns -1).
}

/**************************************
 * Get size of a folder
 **************************************/
uintmax_t EvalSavingPotential::getFolderSize(boost::filesystem::path path) {
    if (!boost::filesystem::is_directory(path)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: %s is not a directory.\n", __FILE__, __func__, __LINE__, path.string());
        return static_cast<uintmax_t>(-1); // Error value otherwise thrown by file_size()
    }

    uintmax_t result = static_cast<uintmax_t>(0);
    boost::filesystem::recursive_directory_iterator end_iter;

    for (boost::filesystem::recursive_directory_iterator iter(path); iter != end_iter; ++iter) {
        if (boost::filesystem::is_regular_file(iter->status())) {
            uintmax_t current_file_size = boost::filesystem::file_size(iter->path());
            if (current_file_size != static_cast<uintmax_t>(-1)) {
                result += current_file_size;
            } else {
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: Skipping erroneous file %s.\n", __FILE__, __func__, __LINE__, iter->path().string());
            }
        }
    }
    return result;
}
/**************************************
 * Get size specifically of a state folder
 **************************************/
uintmax_t EvalSavingPotential::getStateDiskSize(boost::filesystem::path path, unsigned int block_height) {
    if (!boost::filesystem::is_directory(path)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: %s is not a directory.\n", __FILE__, __func__, __LINE__, path.string());
        return static_cast<uintmax_t>(-1); // Error value otherwise thrown by file_size()
    }

    // Derive zero-padded state ID
    std::string state_id = std::to_string(block_height - (block_height % COMPACTION_STEPSIZE));
    state_id.insert(state_id.begin(), 10 - state_id.length(), '0');
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Current state ID: %s.\n", __FILE__, __func__, __LINE__, state_id);

    boost::filesystem::path state_metafile(path / (state_id + ".state"));
    boost::filesystem::path state_chunks(path / "chunks");

    if (!boost::filesystem::is_regular_file(state_metafile)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: %s does not exist.\n", __FILE__, __func__, __LINE__, state_metafile.string());
        return static_cast<uintmax_t>(-1); // Error value otherwise thrown by file_size()
    }

    if (!boost::filesystem::is_directory(state_chunks)) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: %s does not exist.\n", __FILE__, __func__, __LINE__, state_chunks.string());
        return static_cast<uintmax_t>(-1); // Error value otherwise thrown by file_size()
    }

    uintmax_t result = static_cast<uintmax_t>(0);
    result += boost::filesystem::file_size(state_metafile);

    boost::filesystem::recursive_directory_iterator end_iter;

    for (boost::filesystem::recursive_directory_iterator iter(state_chunks); iter != end_iter; ++iter) {
        if (boost::filesystem::is_regular_file(iter->status()) && iter->path().filename().string().compare(0, state_id.size(), state_id) == 0) {
            uintmax_t current_file_size = boost::filesystem::file_size(iter->path());
            if (current_file_size != static_cast<uintmax_t>(-1)) {
                result += current_file_size;
            } else {
                LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: ERROR: Skipping erroneous file %s.\n", __FILE__, __func__, __LINE__, iter->path().string());
            }
        }
    }
    return result;
}


/**************************************
 * Compute blocks folder size
 **************************************/
uintmax_t EvalSavingPotential::getBlocksFolderSize() {
    boost::filesystem::path blocks_path(GetDataDir() / "blocks");

    uintmax_t size_blocks_path = this->getFolderSize(blocks_path);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Blocks folder size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, size_blocks_path);
    return size_blocks_path;
}


/**************************************
 * Compute size of whole state directory
 **************************************/
uintmax_t EvalSavingPotential::getStatesAllFoldersSize() {
    boost::filesystem::path states_path(GetDataDir() / "compaction_states");

    uintmax_t size_states_whole = this->getFolderSize(states_path);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: State folder size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, size_states_whole);
    return size_states_whole;
}

/**************************************
 * Compute size of single state directory
 **************************************/
uintmax_t EvalSavingPotential::getStatesFolderSize(unsigned int block_height) {
    boost::filesystem::path states_path(GetDataDir() / "compaction_states");

    uintmax_t size_state_disk = this->getStateDiskSize(states_path, block_height);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: State disk size (%u) [Byte]: %ju.\n", __FILE__, __func__, __LINE__, block_height, size_state_disk);
    return size_state_disk;
}

/**************************************
 * Compute whole datadir folder size
 **************************************/
uintmax_t EvalSavingPotential::getDatadirFolderSize() {
    boost::filesystem::path datadir_path(GetDataDir());

    uintmax_t size_datadir_path = this->getFolderSize(datadir_path);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Datadir folder size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, size_datadir_path);
    return size_datadir_path;
}

/**************************************
 * Compute datadir folder size w/o states
 **************************************/
uintmax_t EvalSavingPotential::getDatadirFolderSizeNoStates(uintmax_t size_datadir_path, uintmax_t size_states_whole) {
    return size_datadir_path - size_states_whole;
}

/**************************************
 * Compute size of UTXO folder
 **************************************/
uintmax_t EvalSavingPotential::getUtxoFolderSize() {
    boost::filesystem::path chainstate_path(GetDataDir() / "chainstate");

    uintmax_t size_chainstate_path = this->getFolderSize(chainstate_path);
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Chainstate folder size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, size_chainstate_path);
    return size_chainstate_path;
}

/**************************************
 * Compute blockchain size like RPC call
 **************************************/
uint64_t EvalSavingPotential::getRpcBlockchainSize() {
    // Function called to compute "size_on_disk" result in "getblockchaininfo" RPC call
    uint64_t size_blockchain_rpc = CalculateCurrentUsage();
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Blockchain RPC size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, (uintmax_t) size_blockchain_rpc);
    return size_blockchain_rpc;
}

/**************************************
 * Compute accumulated block size
 **************************************/
void EvalSavingPotential::updateAccumulatedBlockSize(std::shared_ptr<const CBlock> block) {
    // We do not actually have a block to handle yet
    if (!block) {
        return;
    }
    this->cumulative_block_sizes_no_witnesses += ::GetSerializeSize(*block, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    this->cumulative_block_sizes += ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
    this->cumulative_block_sizes_since_state_no_witnesses += ::GetSerializeSize(*block, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    this->cumulative_block_sizes_since_state += ::GetSerializeSize(*block, SER_NETWORK, PROTOCOL_VERSION);
}

/**************************************
 * Get accumulated block size
 **************************************/
size_t EvalSavingPotential::getAccumulatedBlockSize(bool witnesses) {
    if (witnesses) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Block size [Byte]: %ju.\n", __FILE__, __func__, __LINE__, (uintmax_t) this->cumulative_block_sizes);
        return this->cumulative_block_sizes;
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Block size (no witnesses) [Byte]: %ju.\n", __FILE__, __func__, __LINE__, (uintmax_t) this->cumulative_block_sizes_no_witnesses);
        return this->cumulative_block_sizes_no_witnesses;
    }
}

/**************************************
 * Get accumulated block size since state
 **************************************/
size_t EvalSavingPotential::getAccumulatedBlockSizeSinceState(bool witnesses) {
    if (witnesses) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Block size since state [Byte]: %ju.\n", __FILE__, __func__, __LINE__, (uintmax_t) this->cumulative_block_sizes);
        return this->cumulative_block_sizes_since_state;
    } else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Block size since state (no witnesses) [Byte]: %ju.\n", __FILE__, __func__, __LINE__, (uintmax_t) this->cumulative_block_sizes_no_witnesses);
        return this->cumulative_block_sizes_since_state_no_witnesses;
    }
}


void EvalSavingPotential::resetAccumulatedBlockSizeSinceState(void) {
    this->cumulative_block_sizes_since_state_no_witnesses = 0;
    this->cumulative_block_sizes_since_state = 0;
}

void EvalSavingPotential::prepareMeasurement(unsigned int block_height, uint32_t block_time) {
    this->stored_size_blocks_path = this->getBlocksFolderSize();
    this->stored_size_chainstate_path = this->getUtxoFolderSize();
    this->stored_size_blockchain_rpc = this->getRpcBlockchainSize();
    this->stored_cumulative_block_sizes_no_witnesses = this->getAccumulatedBlockSize(false);
    this->stored_cumulative_block_sizes = this->getAccumulatedBlockSize(true);
    this->stored_cumulative_block_sizes_since_state_no_witnesses = this->getAccumulatedBlockSizeSinceState(false);
    this->stored_cumulative_block_sizes_since_state = this->getAccumulatedBlockSizeSinceState(true);

    this->stored_block_height = block_height;
    this->stored_block_time = block_time;

    this->resetAccumulatedBlockSizeSinceState();
}

void EvalSavingPotential::doPreparedMeasurement(void) {
    uintmax_t size_state_disk = this->getStatesFolderSize(this->stored_block_height);

    std::stringstream measurement;
    measurement
        << this->n_run << ","
        << this->stored_block_height << ","
        << this->stored_block_time << ","
        << this->stored_size_blocks_path << ","
        << size_state_disk << ","
        << this->stored_size_chainstate_path << ","
        << this->stored_size_blockchain_rpc << ","
        << this->stored_cumulative_block_sizes_no_witnesses << ","
        << this->stored_cumulative_block_sizes << ","
        << this->stored_cumulative_block_sizes_since_state_no_witnesses << ","
        << this->stored_cumulative_block_sizes_since_state;

    this->writeData(measurement.str());

    this->stored_block_height = 0;
    this->stored_block_time = 0;

    this->stored_size_blocks_path = 0;
    this->stored_size_chainstate_path = 0;
    this->stored_size_blockchain_rpc = 0;
    this->stored_cumulative_block_sizes_no_witnesses = 0;
    this->stored_cumulative_block_sizes = 0;
    this->stored_cumulative_block_sizes_since_state_no_witnesses = 0;
    this->stored_cumulative_block_sizes_since_state = 0;
}

/**************************************
 * Gather all values and write to CSV
 **************************************/
void EvalSavingPotential::doMeasurement(unsigned int block_height, uint32_t block_time, bool is_state_height) {

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Gathering size eval data for block height %u.\n", __FILE__, __func__, __LINE__, block_height);

    uintmax_t size_blocks_path = this->getBlocksFolderSize();
    uintmax_t size_states_whole = this->getStatesAllFoldersSize();
    uintmax_t size_state_disk = is_state_height ? this->getStatesFolderSize(block_height) : 0;
    uintmax_t size_chainstate_path = this->getUtxoFolderSize();
    uint64_t size_blockchain_rpc = this->getRpcBlockchainSize();
    size_t cumulative_block_sizes_no_witnesses = this->getAccumulatedBlockSize(false);
    size_t cumulative_block_sizes = this->getAccumulatedBlockSize(true);
    size_t cumulative_block_sizes_since_state_no_witnesses = this->getAccumulatedBlockSizeSinceState(false);
    size_t cumulative_block_sizes_since_state = this->getAccumulatedBlockSizeSinceState(true);

    std::stringstream measurement;
    measurement
        << this->n_run << ","
        << block_height << ","
        << block_time << ","
        << size_blocks_path << ","
        << size_state_disk << ","
        << size_chainstate_path << ","
        << size_blockchain_rpc << ","
        << cumulative_block_sizes_no_witnesses << ","
        << cumulative_block_sizes << ","
        << cumulative_block_sizes_since_state_no_witnesses << ","
        << cumulative_block_sizes_since_state;

    this->writeData(measurement.str());

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: DONE gathering size eval data for block height %u.\n", __FILE__, __func__, __LINE__, block_height);
}


unsigned int EvalSavingPotential::getBlockcount() {
    return this->n_block_count;
}

unsigned int EvalSavingPotential::getMaxBlockcount() {
    return this->n_block_height_max;
}

void EvalSavingPotential::incrementBlockcount() {
    this->n_block_count += 1;
}

bool EvalSavingPotential::onRelevantTailHeight() {
    if (this->n_block_count < COMPACTION_STEPSIZE) {
        return false;
    }

    unsigned int tail_modulus = this->n_block_count % COMPACTION_STEPSIZE;

    for (unsigned int tail_length : this->tail_lengths) {
        if (tail_modulus == tail_length + 1) {
            return true;
        }
    }
    return false;
}

bool EvalSavingPotential::synchronizationFinished() {
    return (this->n_block_count >= this->n_block_height_max);
}







bool initEvaluation() {

    gArgs.DebugArgs();

    std::string command_line_eval_method = gArgs.GetArg("-evalMethod", "");

    // End initialization prematurely if no evaluation method was specified
    if (command_line_eval_method == "") {
        return true;
    } else if (GetEvalDir() == GetDefaultEvalDir()) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: CRITICAL: Evaluation measurements cannot use default evaluation directory for safety reasons. Shutting down.\n", __FILE__, __func__, __LINE__);
        std::cout << "CRITICAL: Evaluation measurements cannot use default evaluation directory for safety reasons. Shutting down." << std::endl;
        StartShutdown();
        return false;
    }


    unsigned int n_run = gArgs.GetArg("-run", 0); // Run ID for measurements that must be instrumented externally
    unsigned int n_runs = gArgs.GetArg("-runs", 2); // Number of desired runs for micro benchmarks

    eval_start_state_height = gArgs.GetArg("-startStateHeight", 0);
    unsigned int n_state_height = gArgs.GetArg("-stateHeight", 0);
    unsigned int n_tail_length = gArgs.GetArg("-tailLength", 0);
    unsigned int n_block_height_max = n_state_height + n_tail_length;

    if (n_state_height > 0 && n_state_height > n_block_height_max) {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Inconsistent state height vs. synchronization height (%d vs %d). Shutting down.\n", __FILE__, __func__, __LINE__, n_state_height, n_block_height_max);
        std::cout << "Inconsistent state height vs. synchronization height (%d vs %d). Shutting down." << std::endl;
        StartShutdown();
        return false;
    }

    // Automatically shut down after measurement if no -shutdownAt was given
    shutdownAtHeight = gArgs.GetArg("-shutdownAt", n_block_height_max);

    // Create evaldir
    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Checking for evaldir.\n", __FILE__, __func__, __LINE__);
    if (!boost::filesystem::exists(GetEvalDir())){
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: evaldir did not exist, creating it.\n", __FILE__, __func__, __LINE__);
        boost::filesystem::create_directories(GetEvalDir());
    }

    evalp_vanilla_synchronization.reset(new EvalVanillaSynchronization(n_run, n_state_height, n_tail_length));
    evalp_compaction_synchronization.reset(new EvalCompactionSynchronization(n_run, n_state_height, n_tail_length));
    evalp_headerchain_download.reset(new EvalHeaderchainDownload(n_run, n_state_height + n_tail_length));
    evalp_saving_potential.reset(new EvalSavingPotential(n_state_height + n_tail_length));
    evalp_state_creation.reset(new EvalStateCreation(n_runs));


    if (command_line_eval_method == "fullSync") {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Eval method: vanilla synchronization\n", __FILE__, __func__, __LINE__);
        evalp_vanilla_synchronization->activate();
    }
    else if (command_line_eval_method == "compactionSync") {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Eval method: compaction synchronization\n", __FILE__, __func__, __LINE__);
        evalp_compaction_synchronization->activate();
    }
    else if (command_line_eval_method == "headerChainDownload") {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Eval method: header chain download only\n", __FILE__, __func__, __LINE__);
        evalp_headerchain_download->activate();
    }
    else if (command_line_eval_method == "createState") {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Eval method: state creation\n", __FILE__, __func__, __LINE__);
        evalp_state_creation->activate();
    }
    else if (command_line_eval_method == "savingPotential") {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Eval method: measure saving potential (n_block_height_max = %u)\n", __FILE__, __func__, __LINE__, evalp_saving_potential->getMaxBlockcount());
        evalp_saving_potential->activate();
        evalp_state_creation->activate();
    }
    else {
        LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: unknown evaluation method\n", __FILE__, __func__, __LINE__);
    }

    LogPrint(BCLog::COMPACTION, "log-compaction: %s,%s,%d: Starting evaluation at state height: %u.\n", __FILE__, __func__, __LINE__, eval_start_state_height);

    return true;
}

#endif
#endif
#endif
