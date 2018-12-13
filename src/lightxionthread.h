//
// Copyright (c) 2015-2018 The ION developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//

#ifndef ION_LIGHTXIONTHREAD_H
#define ION_LIGHTXIONTHREAD_H

#include <atomic>
#include "genwit.h"
#include "accumulators.h"
#include "concurrentqueue.h"
#include "chainparams.h"
#include "main.h"
#include <boost/function.hpp>
#include <boost/thread.hpp>

extern CChain chainActive;
// Max amount of computation for a single request
const int COMP_MAX_AMOUNT = 60 * 24 * 60;


/****** Thread ********/

class CLightWorker{

private:

    concurrentqueue<CGenWit> requestsQueue;
    std::atomic<bool> isWorkerRunning;
    boost::thread threadIns;

public:

    CLightWorker() {
        isWorkerRunning = false;
    }

    enum ERROR_CODES {
        NOT_ENOUGH_MINTS = 0,
        NON_DETERMINED = 1
    };

    bool addWitWork(CGenWit wit) {
        if (!isWorkerRunning) {
            return false;
        }
        requestsQueue.push(wit);
        return true;
    }

    void StartLightXionThread(boost::thread_group& threadGroup) {
        LogPrintf("%s thread start\n", "ion-light-thread");
        threadIns = boost::thread(boost::bind(&CLightWorker::ThreadLightXIONSimplified, this));
    }

    void StopLightXionThread() {
        threadIns.interrupt();
        LogPrintf("%s thread interrupted\n", "ion-light-thread");
    }

private:

    void ThreadLightXIONSimplified() {
        RenameThread("ion-light-thread");
        isWorkerRunning = true;
        while (true) {
            try {

                // Take a breath between requests.. TODO: Add processor usage check here
                MilliSleep(2000);

                // TODO: Future: join several similar requests into one calculation if the filter and denom match..
                CGenWit genWit = requestsQueue.pop();
                LogPrintf("%s pop work for %s \n\n", "ion-light-thread", genWit.toString());

                libzerocoin::ZerocoinParams *params = Params().Zerocoin_Params(false);
                CBlockIndex *pIndex = chainActive[genWit.getStartingHeight()];
                if (!pIndex) {
                    // Rejects only the failed height
                    rejectWork(genWit, genWit.getStartingHeight(), NON_DETERMINED);
                } else {
                    LogPrintf("%s calculating work for %s \n\n", "ion-light-thread", genWit.toString());
                    int blockHeight = pIndex->nHeight;
                    if (blockHeight >= Params().Zerocoin_Block_V2_Start()) {

                        // TODO: The protocol actually doesn't care about the Accumulator..
                        libzerocoin::Accumulator accumulator(params, genWit.getDen(), genWit.getAccWitValue());
                        libzerocoin::PublicCoin temp(params);
                        libzerocoin::AccumulatorWitness witness(params, accumulator, temp);
                        string strFailReason = "";
                        int nMintsAdded = 0;
                        CZerocoinSpendReceipt receipt;

                        list<CBigNum> ret;
                        int heightStop;

                        bool res;
                        try {

                            res = CalculateAccumulatorWitnessFor(
                                    params,
                                    blockHeight,
                                    COMP_MAX_AMOUNT,
                                    genWit.getDen(),
                                    genWit.getFilter(),
                                    accumulator,
                                    witness,
                                    100,
                                    nMintsAdded,
                                    strFailReason,
                                    ret,
                                    heightStop
                            );

                        } catch (NotEnoughMintsException e) {
                            LogPrintStr(std::string("ThreadLightXIONSimplified: ") + e.message + "\n");
                            rejectWork(genWit, blockHeight, NOT_ENOUGH_MINTS);
                            continue;
                        }

                        if (!res) {
                            // TODO: Check if the GenerateAccumulatorWitnessFor can fail for node's fault or it's just because the peer sent an illegal request..
                            rejectWork(genWit, blockHeight, NON_DETERMINED);
                        } else {
                            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                            ss.reserve(ret.size() * 32);

                            ss << genWit.getRequestNum();
                            ss << accumulator.getValue(); // TODO: ---> this accumulator value is not necessary. The light node should get it using the other message..
                            ss << witness.getValue();
                            uint32_t size = ret.size();
                            ss << size;
                            for (CBigNum bnValue : ret) {
                                ss << bnValue;
                            }
                            ss << heightStop;
                            if (genWit.getPfrom()) {
                                LogPrintf("%s pushing message to %s ", "ion-light-thread", genWit.getPfrom()->addrName);
                                genWit.getPfrom()->PushMessage("pubcoins", ss);
                            } else
                                LogPrintf("%s NOT pushing message to %s ", "ion-light-thread", genWit.getPfrom()->addrName);
                        }
                    } else {
                        // Rejects only the failed height
                        rejectWork(genWit, blockHeight, NON_DETERMINED);
                    }
                }
            } catch (std::exception& e) {
                std::cout << "exception in light loop, closing it. " << e.what() << std::endl;
                PrintExceptionContinue(&e, "lightxionthread");
                break;
            }
        }


    }

    // TODO: Think more the peer misbehaving policy..
    void rejectWork(CGenWit& wit, int blockHeight, uint32_t errorNumber) {
        if (wit.getStartingHeight() == blockHeight){
            LogPrintf("%s rejecting work %s , error code: %s", "ion-light-thread", wit.toString(), errorNumber);
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss << wit.getRequestNum();
            ss << errorNumber;
            wit.getPfrom()->PushMessage("pubcoins", ss);
        } else {
            requestsQueue.push(wit);
        }
    }

};

#endif //ION_LIGHTXIONTHREAD_H