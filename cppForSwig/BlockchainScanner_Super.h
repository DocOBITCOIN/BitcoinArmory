////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2017, goatpig.                                              //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //                                      
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _BLOCKCHAINSCANNER_SUPER_H
#define _BLOCKCHAINSCANNER_SUPER_H

#include "Blockchain.h"
#include "lmdb_wrapper.h"
#include "BlockDataMap.h"
#include "Progress.h"
#include "bdmenums.h"
#include "ThreadSafeClasses.h"

#include "SshParser.h"

#include <future>
#include <atomic>
#include <exception>

#define COMMIT_SSH_SIZE 1024 * 1024 * 256ULL
#define LEFTOVER_THRESHOLD 10000000

#ifndef UNIT_TESTS
#define BATCH_SIZE_SUPER 1024 * 1024 * 128ULL
#else
#define BATCH_SIZE_SUPER 1024
#endif

enum BLOCKDATA_ORDER
{
   BD_ORDER_INCREMENT,
   BD_ORDER_DECREMENT
};

////////////////////////////////////////////////////////////////////////////////
struct ThreadSubSshResult
{
   map<BinaryData, map<BinaryData, StoredSubHistory>> subSshMap_;
   unsigned spent_offset_ = 0;
};

////////////////////////////////////////////////////////////////////////////////
struct BlockDataBatch
{
   const BLOCKDATA_ORDER order_;
   atomic<int> blockCounter_;
   const int start_;
   const int end_;

   map<unsigned, shared_ptr<BlockDataFileMap>> fileMaps_;
   map<unsigned, shared_ptr<BlockData>> blockMap_;

   set<unsigned> blockDataFileIDs_;
   BlockDataLoader* blockDataLoader_;
   shared_ptr<Blockchain> blockchain_;

   BlockDataBatch(int start, int end, set<unsigned>& ids,
      BLOCKDATA_ORDER order,
      BlockDataLoader* bdl, shared_ptr<Blockchain> bcPtr) :
      start_(start), end_(end), blockDataFileIDs_(move(ids)),
      blockDataLoader_(bdl), blockchain_(bcPtr), order_(order)
   {}

   void populateFileMap(void);
   shared_ptr<BlockData> getBlockData(unsigned);
   void resetCounter(void);
   shared_ptr<BlockData> getNext(void);
};

////////////////////////////////////////////////////////////////////////////////
struct ParserBatch_Ssh
{
public:
   unique_ptr<BlockDataBatch> bdb_;

   atomic<unsigned> sshKeyCounter_;
   mutex mergeMutex_;

   map<BinaryData, BinaryData> hashToDbKey_;

   map<BinaryDataRef, pair<BinaryWriter, BinaryWriter>> serializedSubSsh_;
   vector<BinaryDataRef> keyRefs_;
   unsigned batch_id_;

   vector<ThreadSubSshResult> txOutSshResults_;
   vector<ThreadSubSshResult> txInSshResults_;

   promise<bool> completedPromise_;
   unsigned count_;
   unsigned spent_offset_;

   chrono::system_clock::time_point parseTxOutStart_;
   chrono::system_clock::time_point parseTxOutEnd_;

   chrono::system_clock::time_point parseTxInStart_;
   chrono::system_clock::time_point parseTxInEnd_;
   chrono::duration<double> serializeSsh_;

   chrono::system_clock::time_point writeSshStart_;
   chrono::system_clock::time_point writeSshEnd_;

   chrono::system_clock::time_point processStart_;
   chrono::system_clock::time_point insertToCommitQueue_;

public:
   ParserBatch_Ssh(unique_ptr<BlockDataBatch> blockDataBatch) :
      bdb_(move(blockDataBatch))
   {}

   void resetCounter(void) { bdb_->resetCounter(); }
};

////////////////////////////////////////////////////////////////////////////////
struct ParserBatch_Spentness
{
   unique_ptr<BlockDataBatch> bdb_;

   map<BinaryData, BinaryData> keysToCommit_;
   map<BinaryData, BinaryData> keysToCommitLater_;
   mutex mergeMutex_;

   promise<bool> prom_;

   ParserBatch_Spentness(unique_ptr<BlockDataBatch> blockDataBatch) :
      bdb_(move(blockDataBatch))
   {}
};

////////////////////////////////////////////////////////////////////////////////
struct StxoRef
{
   uint64_t* valuePtr_;
   uint16_t* indexPtr_;

   BinaryDataRef scriptRef_;
   BinaryDataRef hashRef_;

   unsigned height_;
   uint8_t dup_;
   uint16_t txIndex_, txOutIndex_;

   void unserializeDBValue(const BinaryDataRef&);
   void reset(void) { scriptRef_.reset(); }
   bool isInitialized(void) const { return scriptRef_.isValid(); }

   BinaryData getScrAddressCopy(void) const;
   BinaryData getDBKey(void) const;
};

////////////////////////////////////////////////////////////////////////////////
class BlockchainScanner_Super
{
private:
   int startAt_ = 0;
   bool withUpdateSshHints_ = false;
   bool init_;
   unsigned batch_counter_ = 0;

   shared_ptr<Blockchain> blockchain_;
   LMDBBlockDatabase* db_;
   BlockDataLoader blockDataLoader_;

   BlockingQueue<unique_ptr<ParserBatch_Ssh>> commitQueue_;
   BlockingQueue<pair<BinaryData, BinaryData>> sshBoundsQueue_;
   BlockingQueue<unique_ptr<map<BinaryData, BinaryWriter>>> serializedSshQueue_;
   BlockingQueue<unique_ptr<ParserBatch_Spentness>> spentnessQueue_;

   set<BinaryData> updateSshHints_;

   const unsigned totalThreadCount_;
   const unsigned writeQueueDepth_;
   const unsigned totalBlockFileCount_;
   map<unsigned, HeightAndDup> heightAndDupMap_;
   deque<map<BinaryData, BinaryData>> spentnessLeftOver_;

   BinaryData topScannedBlockHash_;

   ProgressCallback progress_ =
      [](BDMPhase, double, unsigned, unsigned)->void{};
   bool reportProgress_ = false;

   atomic<unsigned> completedBatches_;
   atomic<uint64_t> addrPrefixCounter_;
   
   map<unsigned, unsigned> heightToId_;

private:  
   void commitSshBatch(void);
   void writeSubSsh(ParserBatch_Ssh*);

   void processOutputs(ParserBatch_Ssh*);
   void processOutputsThread(ParserBatch_Ssh*, unsigned);

   void processInputs(ParserBatch_Ssh*);
   void processInputsThread(ParserBatch_Ssh*, unsigned);

   void serializeSubSsh(unique_ptr<ParserBatch_Ssh>);
   void serializeSubSshThread(ParserBatch_Ssh*);

   void writeSpentness(void);

   bool getTxKeyForHash(const BinaryDataRef&, BinaryData&);
   StxoRef getStxoByHash(
      const BinaryDataRef&, uint16_t,
      ParserBatch_Ssh*);
   
   void parseSpentness(ParserBatch_Spentness*);
   void parseSpentnessThread(ParserBatch_Spentness*);

public:
   BlockchainScanner_Super(
      shared_ptr<Blockchain> bc, LMDBBlockDatabase* db,
      BlockFiles& bf, bool init,
      unsigned threadcount, unsigned queue_depth,
      ProgressCallback prg, bool reportProgress) :
      blockchain_(bc), db_(db),
      totalThreadCount_(threadcount), writeQueueDepth_(1/*queue_depth*/),
      blockDataLoader_(bf.folderPath()),
      progress_(prg), reportProgress_(reportProgress),
      totalBlockFileCount_(bf.fileCount()),
      init_(init)
   {}

   void scan(void);
   void scanSpentness(void);
   void updateSSH(bool);
   void undo(Blockchain::ReorganizationState&);

   const BinaryData& getTopScannedBlockHash(void) const
   {
      return topScannedBlockHash_;
   }
};

#endif
