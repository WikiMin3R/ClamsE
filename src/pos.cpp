// Copyright (c) 2012-2013 The PPCoin developers
// Copyright (c) 2014 The BlackCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>

#include "pos.h"
#include "txdb.h"
#include "validation.h"
#include "arith_uint256.h"
#include "hash.h"
#include "timedata.h"
#include "chainparams.h"
#include "script/sign.h"
#include "consensus/params.h"
#include "consensus/consensus.h"

#include "bignum.h"

using namespace std;

int64_t GetWeight(const int64_t &nIntervalBeginning, const int64_t &nIntervalEnd)
{
    // Kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    const Consensus::CParams& params = Params().GetConsensus();
    return min(nIntervalEnd - nIntervalBeginning - params.nStakeMinAge, (int64_t)params.nStakeMaxAge);
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex){
        return error("GetLastStakeModifier: null pindex");
    }
    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier()){
        return error("GetLastStakeModifier: no generation at genesis block %s %s", pindex->ToString(), pindex->GeneratedStakeModifier());
    }
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert (nSection >= 0 && nSection < 64);
    return (Params().GetConsensus().nModifierInterval * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1))));
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection=0; nSection<64; nSection++)
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(vector<pair<int64_t, uint256> >& vSortedByTimestamp, map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop, uint64_t nStakeModifierPrev, const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest;
    *pindexSelected = (const CBlockIndex*) 0;

    BOOST_FOREACH(const PAIRTYPE(int64_t, uint256)& item, vSortedByTimestamp)
    {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString());
        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;
        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;
        // compute the selection hash by hashing its proof-hash and the
        // previous proof-of-stake modifier

        CDataStream ss(SER_GETHASH, 0);
        ss << pindex->hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;
        if (fSelected && hashSelection < hashBest)
        {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
        else if (!fSelected)
        {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*) pindex;
        }
    }

    //LogPrintf("ComputeNextStakeModifier: final selection hash=%s\n", hashBest.ToString());
    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every 
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    const Consensus::CParams& params = Params().GetConsensus();

    nStakeModifier = 0;
    fGeneratedStakeModifier = false;

    if (!pindexPrev)
    {
        fGeneratedStakeModifier = true;
        return true;  // genesis block's modifier is 0
    }
    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime)) {
        return error("ComputeNextStakeModifier: unable to get last modifier");
    }
    if (nModifierTime / Params().GetConsensus().nModifierInterval >= pindexPrev->GetBlockTime() / Params().GetConsensus().nModifierInterval)
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * Params().GetConsensus().nModifierInterval / params.nTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / Params().GetConsensus().nModifierInterval) * Params().GetConsensus().nModifierInterval - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;
    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart)
    {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound=0; nRound<min(64, (int)vSortedByTimestamp.size()); nRound++)
    {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);
        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex)){
            return error("ComputeNextStakeModifier: unable to select block at round %d", nRound);
        }
        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);
        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        //LogPrintf("ComputeNextStakeModifier: selected round=%d, stop=%d, bit=%d, height=%d, pindexTime=%s\n", nRound, nSelectionIntervalStop, pindex->GetStakeEntropyBit(), pindex->nHeight, pindex->nTime);
    }

    //LogPrintf("ComputeNextStakeModifier: new modifier=0x%016x\n", nStakeModifierNew);

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;
    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    const Consensus::CParams& params = Params().GetConsensus();
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("GetKernelStakeModifier() : block not indexed");
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    int64_t nStakeModifierSelectionInterval = GetStakeModifierSelectionInterval();
    const CBlockIndex* pindex = pindexFrom;

    // loop to find the stake modifier later by a selection interval
    while (nStakeModifierTime < pindexFrom->GetBlockTime() + nStakeModifierSelectionInterval)
    {
        if (!pindex->pnext)
        {   // reached best block; may happen if node is behind on block chain
            if (fPrintProofOfStake || (pindex->GetBlockTime() + params.nStakeMinAge - nStakeModifierSelectionInterval > GetAdjustedTime())) {
                return error("GetKernelStakeModifier() : reached best block %s at height %d from block %s",
                    pindex->GetBlockHash().ToString(), pindex->nHeight, hashBlockFrom.ToString());
            }
            else {
                return false;
            }
        }
        pindex = pindex->pnext;
        if (pindex->GeneratedStakeModifier())
        {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
    }
    nStakeModifier = pindex->nStakeModifier;
    return true;
}

// ppcoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget * nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                  future proof-of-stake at the time of the coin's confirmation
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of 
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
static bool CheckStakeKernelHashV1(unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    const Consensus::CParams& params = Params().GetConsensus();
    if (nTimeTx < txPrev.nTime)  // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if ((nTimeBlockFrom + params.nStakeMinAge) > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHashV1() : min age violation");// , nTimeBlockFrom + params.nStakeMinAge, nTimeBlockFrom, nTimeTx, blockFrom.ToString());

    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;

    uint256 hashBlockFrom = blockFrom.GetHash();

    arith_uint256 bnCoinDayWeight = arith_uint256(nValueIn) * GetWeight((int64_t)txPrev.nTime, (int64_t)nTimeTx) / COIN / (24 * 60 * 60);
    targetProofOfStake = ArithToUint256(bnCoinDayWeight * bnTargetPerCoinDay);

    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint64_t nStakeModifier = 0;
    int nStakeModifierHeight = 0;
    int64_t nStakeModifierTime = 0;

    if (!GetKernelStakeModifier(hashBlockFrom, nStakeModifier, nStakeModifierHeight, nStakeModifierTime, fPrintProofOfStake))
        return false;

    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << txPrev.nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (fPrintProofOfStake)
    {
        //LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
        //    nStakeModifier, nStakeModifierHeight,
        //    DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nStakeModifierTime),
        //    mapBlockIndex[hashBlockFrom]->nHeight,
        //    DateTimeStrFormat("%Y-%m-%d %H:%M:%S",blockFrom.GetBlockTime()));
        //LogPrintf("CheckStakeKernelHash() : check modifier=0x%016x nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
        //    nStakeModifier,
        //    nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
         //   hashProofOfStake.ToString());
    }

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) > bnCoinDayWeight * bnTargetPerCoinDay)
        return false;
    if (fDebug && !fPrintProofOfStake)
    {
        //LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
         //   nStakeModifier, nStakeModifierHeight, 
        //    DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nStakeModifierTime),
        //    mapBlockIndex[hashBlockFrom]->nHeight,
        //    DateTimeStrFormat("%Y-%m-%d %H:%M:%S",blockFrom.GetBlockTime()));
        //LogPrintf("CheckStakeKernelHash() : pass modifier=0x%016x nTimeBlockFrom=%u nTxPrevOffset=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
        //    nStakeModifier,
        //    nTimeBlockFrom, nTxPrevOffset, txPrev.nTime, prevout.n, nTimeTx,
        //    hashProofOfStake.ToString());
    }
    return true;
}

// Clam kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHashV2(CBlockIndex* pindexPrev, unsigned int nBits, unsigned int nTimeBlockFrom, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake)
{
    const Consensus::CParams& params = Params().GetConsensus();
    if (nTimeTx < txPrev.nTime) {  // Transaction timestamp violation
        LogPrint("miner", "[STAKE] fail: nTime violation %d %d\n", nTimeTx, txPrev.nTime);
        return error("CheckStakeKernelHash() : nTime violation ");
    }

    if ((nTimeBlockFrom + params.nStakeMinAge) > nTimeTx) { // Min age requirement
        LogPrint("miner", "[STAKE] fail: too young\n");
        return error("CheckStakeKernelHashV2() : min age violation");// %d %d %d %s", nTimeBlockFrom + params.nStakeMinAge, nTimeBlockFrom, nTimeTx, pindexPrev->ToString());
    }

    // Base target
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    int64_t nValueIn = txPrev.vout[prevout.n].nValue;
    CBigNum bnWeight = CBigNum(nValueIn);
    bnTarget *= bnWeight;

    uint64_t nStakeModifier = pindexPrev->nStakeModifier;
    //int nStakeModifierHeight = pindexPrev->nHeight;
    //int64_t nStakeModifierTime = pindexPrev->nTime;


    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier << nTimeBlockFrom << txPrev.nTime << prevout.hash << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    //if (fPrintProofOfStake)
    //{
    //LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from timestamp=%s\n",
     //       nStakeModifier, nStakeModifierHeight,
     //       DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nStakeModifierTime),
     //       DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nTimeBlockFrom));
     //   LogPrintf("CheckStakeKernelHash() : check modifier=0x%016x nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
      //      nStakeModifier,
       //     nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
       //     hashProofOfStake.ToString());
    //}

    // Now check if proof-of-stake hash meets target protocol
    if (CBigNum(hashProofOfStake) > bnTarget) {
        LogPrint("miner", "[STAKE] fail: hash %64s\n", UintToArith256(hashProofOfStake).GetHex());
        LogPrint("miner", "[STAKE]   > target %64s\n", bnTarget.GetHex());
        LogPrint("miner", "[STAKE] fail: hash %64s\n", CBigNum(hashProofOfStake).GetHex());
        LogPrint("miner", "[STAKE]   > target %s\n", bnTarget.GetCompact());
        return false;
    }

    if (fPrintProofOfStake) {
        LogPrint("miner", "[STAKE] PASS: hash %64s\n", UintToArith256(hashProofOfStake).GetHex());
        LogPrint("miner", "[STAKE]  <= target %64s\n", bnTarget.GetHex());
    }

    if (fDebug && !fPrintProofOfStake)
    {
            //LogPrintf("CheckStakeKernelHash() : using modifier 0x%016x at height=%d timestamp=%s for block from timestamp=%s\n",
            //nStakeModifier, nStakeModifierHeight,
            //DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nStakeModifierTime),
            //DateTimeStrFormat("%Y-%m-%d %H:%M:%S",nTimeBlockFrom));
        //LogPrintf("CheckStakeKernelHash() : pass modifier=0x%016x nTimeBlockFrom=%u nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
            //nStakeModifier,
            //nTimeBlockFrom, txPrev.nTime, prevout.n, nTimeTx,
            //hashProofOfStake.ToString());
    }

    return true;
}


// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, CValidationState& state, const CTransaction& tx, unsigned int nBits, uint256& hashProofOfStake, uint256& targetProofOfStake, CCoinsViewCache& view, CBlockTreeDB& db, const Consensus::CParams& consensusParams)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    uint256 hashBlock;
    CTransactionRef txPrevRef;
    // Kernel (input 0) must match the stake hash target (nBits)
    const CTxIn& txin = tx.vin[0];
    Coin coinPrev;

    if(!view.GetCoin(txin.prevout, coinPrev)){
        LogPrint("miner", "CheckProofOfStake() : Stake prevout does not exist %s\n", txin.prevout.hash.ToString());
        return state.DoS(100, error("CheckProofOfStake() : Stake prevout does not exist %s", txin.prevout.hash.ToString()));
    }

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        LogPrint("miner", "CheckProofOfStake() : Stake prevout does not exist %s\n", txin.prevout.hash.ToString());
        return state.DoS(100, error("CheckProofOfStake() : Block at height %i for prevout can not be loaded", coinPrev.nHeight));
    }

    CBlock block;
    if (!ReadBlockFromDisk(block, blockFrom, consensusParams))
        return state.DoS(100, error("%s: CheckProofOfStake()", __func__), REJECT_INVALID, "block-not-found");

    if (!GetTransaction(txin.prevout.hash, txPrevRef, Params().GetConsensus(), hashBlock, true))
        return state.DoS(1, error("%s: prevout-not-in-chain", __func__), REJECT_INVALID, "prevout-not-in-chain");
    const CTransaction& txPrev = *txPrevRef;

    int nHeight = pindexPrev->nHeight;
    int nTxOffset = 0;
    pblocktree->ReadTxOffsetIndex(nHeight, nTxOffset);

    // Verify signature
    if (!VerifySignature(coinPrev, txin.prevout.hash, tx, 0, SCRIPT_VERIFY_NONE)){
        LogPrint("miner", "CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s\n", tx.GetHash().ToString(), hashProofOfStake.ToString());
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));
    }


    if (!CheckStakeKernelHash(pindexPrev, nBits, block, nTxOffset, txPrev, txin.prevout, tx.nTime, hashProofOfStake, targetProofOfStake, fDebug)) {
        LogPrint("miner", "CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s\n", tx.GetHash().ToString(), hashProofOfStake.ToString());
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s", tx.GetHash().ToString(), hashProofOfStake.ToString())); // may occur during initial download or if behind on block chain sync
    }

    return true;
}



bool CheckStakeKernelHash(CBlockIndex* pindexPrev, unsigned int nBits, const CBlock& blockFrom, unsigned int nTxPrevOffset, const CTransaction& txPrev, const COutPoint& prevout, unsigned int nTimeTx, uint256& hashProofOfStake, uint256& targetProofOfStake, bool fPrintProofOfStake, const Consensus::CParams& consensusParams)
{
    if (pindexPrev->nHeight + 1 > consensusParams.nProtocolV2Height) {
        return CheckStakeKernelHashV2(pindexPrev, nBits, blockFrom.GetBlockTime(), txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake);
    } 
    else {
        return CheckStakeKernelHashV1(nBits, blockFrom, nTxPrevOffset, txPrev, prevout, nTimeTx, hashProofOfStake, targetProofOfStake, fPrintProofOfStake);
    }
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int nHeight, int64_t nTimeBlock, int64_t nTimeTx)
{
    if (nHeight > Params().GetConsensus().nProtocolV2Height)
        return (nTimeBlock == nTimeTx) && ((nTimeTx & STAKE_TIMESTAMP_MASK) == 0);
    else
        return (nTimeBlock == nTimeTx);
}

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, const COutPoint& prevout, CCoinsViewCache& view, CBlockTreeDB& db, unsigned int txTime)
{
    uint256 hashProofOfStake, targetProofOfStake;
    const Consensus::CParams& params = Params().GetConsensus();
    CValidationState state;

    uint256 hashBlock;
    CTransactionRef txPrevRef;

    Coin coinPrev;
    if(!view.GetCoin(prevout, coinPrev)){
        return false;
    }

    if(pindexPrev->nHeight + 1 - coinPrev.nHeight < Params().GetConsensus().nCoinbaseMaturity){
        return false;
    }

    CBlockIndex* blockFrom = pindexPrev->GetAncestor(coinPrev.nHeight);
    if(!blockFrom) {
        return false;
    }

    if(coinPrev.IsSpent()){
        return false;
    }

    CBlock block;
    if (!ReadBlockFromDisk(block, blockFrom, params))
        return state.DoS(100, error("%s: CheckProofOfStake()", __func__), REJECT_INVALID, "block-not-found");

    int nHeight=pindexPrev->nHeight;
    int nTxOffset=0;
    pblocktree->ReadTxOffsetIndex(nHeight, nTxOffset);

    if (!GetTransaction(prevout.hash, txPrevRef, Params().GetConsensus(), hashBlock, true))
        return state.DoS(1, error("%s: prevout-not-in-chain", __func__), REJECT_INVALID, "prevout-not-in-chain");
    const CTransaction& txPrev = *txPrevRef;

    return CheckStakeKernelHash(pindexPrev, nBits, block, nTxOffset, txPrev, prevout,
                                txTime, hashProofOfStake, targetProofOfStake, false, params);
}
















