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

static uint64_t PC = 0;

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

// count of load-use stalls
static uint64_t loadStallCount = 0;

// run the simulator for a certain number of cycles
// return SUCCESS if reaching desired cycles.
// return HALT if the simulator halts on 0xfeedfeed

Status runCycles(uint64_t cycles) {
    uint64_t count = 0;
    auto status = SUCCESS;
    PipeState pipeState = {
        0,
    };


    while (cycles == 0 || count < cycles) {

            pipeState.cycle = cycleCount;
            count++;
            cycleCount++;
        
            // Save Previous Cycle Pipeline State
            Simulator::Instruction prevIFInst = pipelineInfo.ifInst;
            Simulator::Instruction prevIDInst = pipelineInfo.idInst;
            Simulator::Instruction prevEXInst = pipelineInfo.exInst;
            Simulator::Instruction prevMEMInst = pipelineInfo.memInst;

            // Decode IF Stage
            Simulator::Instruction decode = simulator->simID(PC);

            bool stall = false;

            // Check for data hazards 
            if (isLoad(prevIDInst)) {
                uint64_t loadRd = prevIDInst.rd;

                if (isStore(decode)) {
                    if (decode.readsRs1 && decode.rs1 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }
                }

                else {
                    if (decode.readsRs1 && decode.rs1 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }
                    if (decode.readsRs2 && decode.rs2 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }
                }
            }

            // Check for data hazards with branches/jumps
            if (!stall && isBranchOrJump(decode)) {
                if (writesREG(prevEXInst) && !isLoad(prevEXInst)) {
                    uint64_t aluRd = prevIDInst.rd;

                    if (decode.readsRs1 && decode.rs1 == aluRd != 0) {
                        stall = true;
                    }

                    if (decode.readsRs2 && decode.rs2 == aluRd != 0) {
                        stall = true;
                    }
                }
            }

            // Check for data hazards with stores
            if (!stall && isBranchOrJump(decode)) {
                // Check EX stage
                if (isLoad(prevIDInst)) {
                    uint64_t loadRd = prevIDInst.rd;

                    if (decode.readsRs1 && decode.rs1 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }

                    if (decode.readsRs2 && decode.rs2 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }
            }

                // Check MEM stage
                else if (isLoad(prevEXInst)) {
                    uint64_t loadRd = prevEXInst.rd;

                    if (decode.readsRs1 && decode.rs1 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
                    }

                    if (decode.readsRs2 && decode.rs2 == loadRd != 0) {
                        stall = true;
                        loadStallCount++;
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

        // Memory Stage
        pipelineInfo.memInst = simulator->simMEM(prevEXInst);

        // Execute Stage
        if (stall) {
            pipelineInfo.exInst = nop(BUBBLE);
        } else {
            pipelineInfo.exInst = simulator->simEX(prevIDInst);
        }

        // Instruction Decode Stage
        if (stall) {
            pipelineInfo.idInst = prevIDInst;
        } else {
            pipelineInfo.idInst = decode;
        }

        // Check for Branch
        book taken = false;
        if (!stall && isBranchOrJump(pipelineInfo.idInst)) {
            if (pipelineInfo.idInst.nextPC != pipelineInfo.idInst.PC + 4) {
                taken = true;
            }
        }

        // Instruction Fetch Stage
        if (stall) {
            pipelineInfo.ifInst = prevIFInst;
        } 
        
        else if (taken) {
            pipelineInfo.ifInst.status = SQUASHED;
            PC = pipelineInfo.idInst.nextPC;
            pipelineInfo.ifInst = simulator->simIF(PC);
            PC += 4;
        }

        else {
            pipelineInfo.ifInst = simulator->simIF(PC);
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
    SimulationStats stats{simulator->getDin(),  cycleCount, 0, 0, 0, 0, 0};  // TODO incomplete implementation
    dumpSimStats(stats, output);
    return SUCCESS;
}
