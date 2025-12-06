#include "cycle.h"

#include <iostream>
#include <memory>
#include <string>

#include "Utilities.h"
#include "cache.h"
#include "simulator.h"

static Simulator* simulator = nullptr;
static Cache* iCache = nullptr;
static Cache* dCache = nullptr;
static std::string output;
static uint64_t cycleCount = 0;
// count of load-use stalls
static uint64_t loadStallCount = 0;
// remaining stall cycles
static uint64_t remainingStallCycles = 0;
static uint64_t PC = 0;

// Exception Address
#define EXC_HANDLER 0x8000

/**TODO: Implement pipeline simulation for the RISCV machine in this file.
 * A basic template is provided below that doesn't account for any hazards.
 */

Simulator::Instruction nop(StageStatus status) {
    Simulator::Instruction nop;
    nop.instruction = 0x00000013;
    nop.isLegal = true;
    nop.isNop = true;
    nop.status = status;
    return nop;
}

static struct PipelineInfo {
    Simulator::Instruction ifInst = nop(IDLE);
    Simulator::Instruction idInst = nop(IDLE);
    Simulator::Instruction exInst = nop(IDLE);
    Simulator::Instruction memInst = nop(IDLE);
    Simulator::Instruction wbInst = nop(IDLE);
} pipelineInfo;


// initialize the simulator
Status initSimulator(CacheConfig& iCacheConfig, CacheConfig& dCacheConfig, MemoryStore* mem,
                     const std::string& output_name) {
    output = output_name;
    simulator = new Simulator();
    simulator->setMemory(mem);
    iCache = new Cache(iCacheConfig, I_CACHE);
    dCache = new Cache(dCacheConfig, D_CACHE);
    return SUCCESS;
}

// return true if the instruction is a memory access
static bool isLoad(const Simulator::Instruction& inst) {
    return inst.readsMem && inst.isLegal && !inst.isNop 
            && inst.status != BUBBLE && inst.status != SQUASHED;
}

// return true if the instruction is a memory access
static bool isStore(const Simulator::Instruction& inst) {
    return inst.writesMem && inst.isLegal && !inst.isNop 
           && inst.status != BUBBLE && inst.status != SQUASHED;
}

// return true if the instruction writes to a register
static bool writesREG(const Simulator::Instruction& inst) {
    return inst.writesRd && inst.rd != 0 &&inst.isLegal && !inst.isNop 
           && inst.status != BUBBLE && inst.status != SQUASHED;
}

static bool isBranchOrJump(const Simulator::Instruction& inst) {
    return (inst.opcode == OP_BRANCH || inst.opcode == OP_JAL || inst.opcode == OP_JALR)
           && inst.isLegal && !inst.isNop 
           && inst.status != BUBBLE && inst.status != SQUASHED;
}


// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles) {
    uint64_t fowardValue;
    uint64_t count = 0;
    auto status = SUCCESS;
    PipeState pipeState = {
        0,
    };


    while (cycles == 0 || count < cycles) {

            pipeState.cycle = cycleCount;

            // Angel debuggin
            std::cout << "[DEBUG] Starting cycle " << cycleCount << std::endl;

            count++;
            cycleCount++;
        
            // Save Previous Cycle Pipeline State
            Simulator::Instruction prevIFInst = pipelineInfo.ifInst;
            Simulator::Instruction prevIDInst = pipelineInfo.idInst;
            Simulator::Instruction prevEXInst = pipelineInfo.exInst;
            Simulator::Instruction prevMEMInst = pipelineInfo.memInst;
            Simulator::Instruction prevWBInst = pipelineInfo.wbInst;

            // Speculative Decode
            Simulator::Instruction spec_decode = simulator->simID(prevIFInst);

            bool stall = false;
            int newStallCycles = 0;
            bool illegalExc = false;
            bool memExc = false;


            if (remainingStallCycles > 0) {
                stall = true;
                remainingStallCycles--;
            }

            else {
            // Check for data hazards 
            if (isLoad(prevIDInst)) {
                uint64_t loadRd = prevIDInst.rd;

                if (isStore(spec_decode)) {
                    if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0)) {
                        stall = true;
                        loadStallCount++;
                    }
                }

                else if (isBranchOrJump(spec_decode)) {

                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0)) {
                        stallRequired = true;
                    }
                    if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0)) {
                        stallRequired = true;
                    }

                    if (stallRequired) {
                        stall = true;
                        remainingStallCycles = 1;
                        loadStallCount++;
                }
            }

            else {

                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0)) {
                        stallRequired = true;
                    }
                    if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0)) {
                        stallRequired = true;
                    }

                    if (stallRequired) {
                        stall = true;
                        loadStallCount++;
                }
            }
        }


            if (!stall && isBranchOrJump(spec_decode) && isLoad(prevEXInst)) {
                uint64_t loadRd = prevEXInst.rd;
                bool stallRequired = false;

                if ((spec_decode.readsRs1 && spec_decode.rs1 == loadRd) && (loadRd != 0)) {
                    stallRequired = true;
                }

                if ((spec_decode.readsRs2 && spec_decode.rs2 == loadRd) && (loadRd != 0)) {
                    stallRequired= true;
                }

                if (stallRequired) {
                    stall = true;
                    loadStallCount++;
                }
            }

            // Check for data hazards with branches/jumps
            if (!stall && isBranchOrJump(spec_decode)) {
                if (writesREG(prevIDInst) && !isLoad(prevIDInst)) {
                    uint64_t aluRd = prevIDInst.rd;
                    bool stallRequired = false;

                    if ((spec_decode.readsRs1 && spec_decode.rs1 == aluRd) && ( aluRd!= 0)) {
                        stallRequired = true;
                    }

                    if ((spec_decode.readsRs2 && spec_decode.rs2 == aluRd) && ( aluRd!= 0)) {
                        stallRequired = true;
                    }

                    if (stallRequired) {
                        stall = true;
                    }
                }
            }
        }

        // Write Back Stage
        pipelineInfo.wbInst = simulator->simWB(prevMEMInst);

        // Halt check
        if (pipelineInfo.wbInst.isHalt) {
            pipeState.ifPC = pipelineInfo.ifInst.PC;
            pipeState.ifStatus = pipelineInfo.ifInst.status;
            pipeState.idInstr = pipelineInfo.idInst.instruction;
            pipeState.idStatus = pipelineInfo.idInst.status;
            pipeState.exInstr = pipelineInfo.exInst.instruction;
            pipeState.exStatus = pipelineInfo.exInst.status;
            pipeState.memInstr = pipelineInfo.memInst.instruction;
            pipeState.memStatus = pipelineInfo.memInst.status;
            pipeState.wbInstr = pipelineInfo.wbInst.instruction;
            pipeState.wbStatus = pipelineInfo.wbInst.status;
            dumpPipeState(pipeState, output);
            status = HALT;
            break;
        }

        // Foward WB -> MEM
        if (isStore(prevEXInst) && writesREG(prevMEMInst)) {
            uint64_t wbRd = prevMEMInst.rd;

            if ((prevEXInst.rs2 == wbRd) && (wbRd != 0)) {
                if (isLoad(prevMEMInst)) {
                    prevEXInst.op2Val = prevMEMInst.memResult;
                } 
                
                else {
                    prevEXInst.op2Val = prevMEMInst.arithResult;
                }
            }
        }

        // Memory Stage
        pipelineInfo.memInst = simulator->simMEM(prevEXInst);

        if (pipelineInfo.memInst.memException) {
            memExc = true;
            pipelineInfo.memInst.status = SQUASHED;
        }

        // Forward from MEM → EX
        if (writesREG(prevMEMInst)) {
            uint64_t memRd = prevMEMInst.rd;
            
            if (isLoad(prevMEMInst)) {
                fowardValue = prevMEMInst.memResult;
            } 
            
            else {
                fowardValue = prevMEMInst.arithResult;
            }

            if (prevIDInst.readsRs1 && prevIDInst.rs1 == memRd && memRd != 0) {
                prevIDInst.op1Val = fowardValue;  
            }
            if (prevIDInst.readsRs2 && prevIDInst.rs2 == memRd && memRd != 0) {
                prevIDInst.op2Val = fowardValue;
            }
        }

        // Forward from EX → EX
        if (writesREG(prevEXInst)) {
            uint64_t exRd = prevEXInst.rd;

            if (prevIDInst.readsRs1 && prevIDInst.rs1 == exRd && exRd != 0) {
                prevIDInst.op1Val = prevEXInst.arithResult;
            }
            if (prevIDInst.readsRs2 && prevIDInst.rs2 == exRd && exRd != 0) {
                prevIDInst.op2Val = prevEXInst.arithResult;
            }
        }
        // Execute Stage
        pipelineInfo.exInst = simulator->simEX(prevIDInst);

        // Instruction Decode Stage
        if (stall) {
            pipelineInfo.idInst = nop(BUBBLE);
        } else {
            pipelineInfo.idInst = spec_decode;
        }

        if (!pipelineInfo.idInst.isLegal && !pipelineInfo.idInst.isNop &&
            pipelineInfo.idInst.status != BUBBLE && 
            pipelineInfo.idInst.status != SQUASHED &&
            !pipelineInfo.idInst.isHalt) {
            illegalExc = true;
            pipelineInfo.idInst.status = SQUASHED;
        }

        // Check for Branch
        bool taken = false;
        if (!stall && isBranchOrJump(pipelineInfo.idInst)) {
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4) {
                taken = true;
            }
        }

        // Instruction Fetch Stage
        if (illegalExc) {
            pipelineInfo.ifInst.status = SQUASHED;
            PC = EXC_HANDLER;
        } 
        
        else if (memExc) {
            pipelineInfo.ifInst.status = SQUASHED;
            pipelineInfo.idInst.status = SQUASHED;
            pipelineInfo.exInst.status = SQUASHED;
            PC = EXC_HANDLER;
        }

        else if (stall) {

            // Angel debuggin
            std::cout << "[DEBUG] STALL at cycle " << cycleCount << std::endl;

            pipelineInfo.ifInst = prevIFInst;

            if (isBranchOrJump(spec_decode)) {
                pipelineInfo.ifInst.status = SPECULATIVE;
            }
        } 
        
        else if (taken) {

            // Angel debuggin
            std::cout << "[DEBUG] Branch taken → PC = 0x" 
                << std::hex << pipelineInfo.idInst.nextPC 
                << std::dec << std::endl;
                
            prevIFInst.status = SQUASHED;
            pipelineInfo.ifInst = prevIFInst;

            PC = pipelineInfo.idInst.nextPC;
        }

        else {
            pipelineInfo.ifInst = simulator->simIF(PC);

            if (isBranchOrJump(pipelineInfo.idInst)) {
                pipelineInfo.ifInst.status = SPECULATIVE;
            }

            // Angel debuggin
            std::cout << "[DEBUG] PC moves to " << std::hex << PC+4 << std::dec << std::endl;

            PC += 4;
        }
    }

    // Dump Pipeline
    pipeState.ifPC = pipelineInfo.ifInst.PC;
    pipeState.ifStatus = pipelineInfo.ifInst.status;
    pipeState.idInstr = pipelineInfo.idInst.instruction;
    pipeState.idStatus = pipelineInfo.idInst.status;
    pipeState.exInstr = pipelineInfo.exInst.instruction;
    pipeState.exStatus = pipelineInfo.exInst.status;
    pipeState.memInstr = pipelineInfo.memInst.instruction;
    pipeState.memStatus = pipelineInfo.memInst.status;
    pipeState.wbInstr = pipelineInfo.wbInst.instruction;
    pipeState.wbStatus = pipelineInfo.wbInst.status;

    if (status != HALT) {
        dumpPipeState(pipeState, output);
    }
    return status;
}

// run till halt (call runCycles() with cycles == 1 each time) until
// status tells you to HALT or ERROR out
Status runTillHalt() {
    Status status;
    while (true) {
        status = static_cast<Status>(runCycles(1));
        if (status == HALT) break;
    }
    return status;
}

// dump the state of the simulator
Status finalizeSimulator() {
    simulator->dumpRegMem(output);
    SimulationStats stats{
        simulator->getDin(),
        cycleCount,
        iCache->getHits(),
        iCache->getMisses(),
        dCache->getHits(),
        dCache->getMisses(),
        loadStallCount
};    dumpSimStats(stats, output);
    return SUCCESS;
}
