#pragma once
#include "common.h"
#include "config.h"
#include "ras.h"
#include "checkpoint_buffer.h"
namespace component
{
		//---------------------------关于如何获取是否跳转的预测--------------------------//
		//采用基于全局历史和局部历史的混合分支预测模式
		//gshare表示全局历史，local表示局部历史
		//pht pattern history table 存储每个模式下的历史跳转状态，用两位计数器表示
		
		//这里索引gshare_pht用的是gshare_global_history ^ pc_p1 pc_p1是pc的低几位
		//gshare_global_history实际上是ghr，global history register,用来记录全局分支历史
		//用pc和全局分支历史配合来索引pht
		
		//索引local_pht用的是bht_value ^ pc_p1
		//bht_value是来自local_bht_retired,lobal_bht实际上是一个bhr的表，bhr branch history register，记录某一条分支指令过去的执行结果，bhr和ghr类似
		//从lobal_bht中索引bht_value的索引值也是pc_p1,也即代表一条指令
		
		//而实际最终判断是否跳转是综合考虑全局历史和局部历史来选择的，根据cpht来选择是选用全局的结果还是局部的结果，
		//索引cpht使用的索引与全局相同gshare_global_history ^ pc_p1 pc_p1
		//至于cpht的更新，cpht中索引到的值为0、1表示选用全局预测的结果，2、3表示选用局部预测的结果，所以更新cpht的时候也根据选用全局预测结果还是局部预测结果而不同，选用全局且预测正确时-1,选用局部且预测正确时+1

		//------------------------------关于获取预测跳转的地址---------------------------//
		//
		
    class branch_predictor : public if_reset_t
    {
        private:
            uint32_t gshare_global_history = 0;
            uint32_t gshare_global_history_retired = 0;
            uint32_t gshare_pht[GSHARE_PHT_SIZE];
            void gshare_global_history_update(bool jump, bool hit)
            {
                gshare_global_history_retired = ((gshare_global_history_retired << 1) & GSHARE_GLOBAL_HISTORY_MASK) | (jump ? 1 : 0);
                if(!hit)
                {
                    gshare_global_history = gshare_global_history_retired;
                }
            }
            void gshare_global_history_update_bru_fix(checkpoint_t &cp, bool jump, bool hit)
            {
                uint32_t gshare_global_history_bru = ((cp.global_history << 1) & GSHARE_GLOBAL_HISTORY_MASK) | (jump ? 1 : 0);
                if(!hit)
                {
                    gshare_global_history = gshare_global_history_bru;
                }
            }
            void gshare_global_history_update_guess(bool jump)
            {
                gshare_global_history = ((gshare_global_history << 1) & GSHARE_GLOBAL_HISTORY_MASK) | (jump ? 1 : 0);
            }
            bool gshare_get_prediction(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + GSHARE_PC_P2_ADDR_WIDTH)) & GSHARE_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & GSHARE_PC_P2_ADDR_MASK;
                uint32_t pht_addr = gshare_global_history ^ pc_p1;
                return gshare_pht[pht_addr] >= 2;
            }
            void gshare_update_prediction(uint32_t pc, bool jump, bool hit)
            {
                uint32_t pc_p1 = (pc >> (2 + GSHARE_PC_P2_ADDR_WIDTH)) & GSHARE_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & GSHARE_PC_P2_ADDR_MASK;
                uint32_t pht_addr = gshare_global_history_retired ^ pc_p1;
                if(jump)
                {
                    if(gshare_pht[pht_addr] < 3)
                    {
                        gshare_pht[pht_addr]++;
                    }
                }
                else
                {
                    if(gshare_pht[pht_addr] > 0)
                    {
                        gshare_pht[pht_addr]--;
                    }
                }
                gshare_global_history_update(jump, hit);
            }
            uint32_t local_bht[LOCAL_BHT_SIZE];
            uint32_t local_bht_retired[LOCAL_BHT_SIZE];
            uint32_t local_pht[LOCAL_PHT_SIZE];
            void local_update_prediction(uint32_t pc, bool jump, bool hit)
            {
                uint32_t pc_p1 = (pc >> (2 + LOCAL_PC_P2_ADDR_WIDTH)) & LOCAL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & LOCAL_PC_P2_ADDR_MASK;
                uint32_t bht_value = local_bht_retired[pc_p1];
                uint32_t pht_addr = bht_value ^ pc_p1;
                local_bht_retired[pc_p1] = ((local_bht_retired[pc_p1] << 1) & LOCAL_BHT_WIDTH_MASK) | (jump ? 1 : 0);
                
                if(jump)
                {
                    if(local_pht[pht_addr] < 3)
                    {
                        local_pht[pht_addr]++;
                    }
                }
                else
                {
                    if(local_pht[pht_addr] > 0)
                    {
                        local_pht[pht_addr]--;
                    }
                }
                if(!hit)
                {
                    local_bht[pc_p1] = local_bht_retired[pc_p1];
                }
            }
            uint32_t local_get_bht_value(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + LOCAL_PC_P2_ADDR_WIDTH)) & LOCAL_PC_P1_ADDR_MASK;
                return local_bht[pc_p1];
            }
            void local_update_prediction_bru_fix(component::checkpoint_t &cp, uint32_t pc, bool jump, bool hit)
            {
                uint32_t pc_p1 = (pc >> (2 + LOCAL_PC_P2_ADDR_WIDTH)) & LOCAL_PC_P1_ADDR_MASK;
                uint32_t local_history_bru = ((cp.local_history << 1) & LOCAL_BHT_WIDTH_MASK) | (jump ? 1 : 0);
                if(!hit)
                {
                    local_bht[pc_p1] = local_history_bru;
                }
            }
            void local_update_prediction_guess(uint32_t pc, bool jump)
            {
                uint32_t pc_p1 = (pc >> (2 + LOCAL_PC_P2_ADDR_WIDTH)) & LOCAL_PC_P1_ADDR_MASK;
                local_bht[pc_p1] = ((local_bht[pc_p1] << 1) & LOCAL_BHT_WIDTH_MASK) | (jump ? 1 : 0);
            }
            bool local_get_prediction(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + LOCAL_PC_P2_ADDR_WIDTH)) & LOCAL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & LOCAL_PC_P2_ADDR_MASK;
                uint32_t bht_value = local_bht[pc_p1];
                uint32_t pht_addr = bht_value ^ pc_p1;
                return local_pht[pht_addr] >= 2;
            }
            uint32_t cpht[GSHARE_PHT_SIZE];
            
            void cpht_update_prediction(uint32_t pc, bool hit)
            {
                uint32_t pc_p1 = (pc >> (2 + GSHARE_PC_P2_ADDR_WIDTH)) & GSHARE_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & GSHARE_PC_P2_ADDR_MASK;
                uint32_t cpht_addr = gshare_global_history ^ pc_p1;
                
                {
                    if(hit && (cpht[cpht_addr] > 0))
                    {
                        cpht[cpht_addr]--;
                    }
                    else if(!hit)
                    {
                        cpht[cpht_addr]++;
                    }
                }
                {
                    if(hit && (cpht[cpht_addr] < 3))
                    {
                        cpht[cpht_addr]++;
                    }
                    else if(!hit)
                    {
                        cpht[cpht_addr]--;
                    }
                }
            }
            bool cpht_get_prediction(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + GSHARE_PC_P2_ADDR_WIDTH)) & GSHARE_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & GSHARE_PC_P2_ADDR_MASK;
                uint32_t cpht_addr = gshare_global_history ^ pc_p1;
                return cpht[cpht_addr] <= 1;
            }
            component::ras main_ras;
            uint32_t call_global_history = 0;
            uint32_t call_target_cache[CALL_TARGET_CACHE_SIZE];
            void call_global_history_update(bool jump)
            {
                call_global_history = ((call_global_history << 1) & CALL_GLOBAL_HISTORY_MASK) | (jump ? 1 : 0);
            }
            void call_update_prediction(uint32_t pc, bool jump, uint32_t target)
            {
                uint32_t pc_p1 = (pc >> (2 + CALL_PC_P2_ADDR_WIDTH)) & CALL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & CALL_PC_P2_ADDR_MASK;
                uint32_t target_cache_addr = call_global_history ^ pc_p1;
                call_target_cache[target_cache_addr] = target;
                call_global_history_update(jump);
            }
            uint32_t call_get_prediction(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + CALL_PC_P2_ADDR_WIDTH)) & CALL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & CALL_PC_P2_ADDR_MASK;
                uint32_t target_cache_addr = call_global_history ^ pc_p1;
                return call_target_cache[target_cache_addr];
            }
            uint32_t normal_global_history = 0;
            uint32_t normal_target_cache[NORMAL_TARGET_CACHE_SIZE];
            void normal_global_history_update(bool jump)
            {
                normal_global_history = ((normal_global_history << 1) & NORMAL_GLOBAL_HISTORY_MASK) | (jump ? 1 : 0);
            }
            void normal_update_prediction(uint32_t pc, bool jump, uint32_t target)
            {
                uint32_t pc_p1 = (pc >> (2 + NORMAL_PC_P2_ADDR_WIDTH)) & NORMAL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & NORMAL_PC_P2_ADDR_MASK;
                uint32_t target_cache_addr = normal_global_history ^ pc_p1; 
                normal_target_cache[target_cache_addr] = target;
                normal_global_history_update(jump);
            }
            uint32_t normal_get_prediction(uint32_t pc)
            {
                uint32_t pc_p1 = (pc >> (2 + NORMAL_PC_P2_ADDR_WIDTH)) & NORMAL_PC_P1_ADDR_MASK;
                uint32_t pc_p2 = (pc >> 2) & NORMAL_PC_P2_ADDR_MASK;
                uint32_t target_cache_addr = normal_global_history ^ pc_p1;
                return normal_target_cache[target_cache_addr];
            }
            enum class sync_request_type_t
            {
                update_prediction
            };
            typedef struct sync_request_t
            {
                sync_request_type_t req;
                uint32_t pc;
                uint32_t instruction;
                bool jump;
                uint32_t next_pc;
                bool hit;
            }sync_request_t;
            std::queue<sync_request_t> sync_request_q;
        public:
            {
                gshare_global_history = 0;
                memset(gshare_pht, 0, sizeof(gshare_pht));
                memset(local_bht, 0, sizeof(local_bht));
                memset(local_pht, 0, sizeof(local_pht));
                memset(cpht, 0, sizeof(cpht));
                call_global_history = 0;
                memset(call_target_cache, 0, sizeof(call_target_cache));
                normal_global_history = 0;
                memset(normal_target_cache, 0, sizeof(normal_target_cache));
            }
            virtual void reset()
            {
                gshare_global_history = 0;
                gshare_global_history_retired = 0;
                memset(gshare_pht, 0, sizeof(gshare_pht));
                memset(local_bht, 0, sizeof(local_bht));
                memset(local_pht, 0, sizeof(local_pht));
                memset(cpht, 0, sizeof(cpht));
                call_global_history = 0;
                memset(call_target_cache, 0, sizeof(call_target_cache));
                normal_global_history = 0;
                memset(normal_target_cache, 0, sizeof(normal_target_cache));
                clear_queue(sync_request_q);
                for(auto i = 0;i < GSHARE_PHT_SIZE;i++)
                {
                    gshare_pht[i] = 0x00;
                }
            }
            void trace_pre()
            {
                
                for(auto i = 0;i < COMMIT_WIDTH;i++)
                {
                }
                
                for(auto i = 0;i < COMMIT_WIDTH;i++)
                {
                }
            }
            void trace_post()
            {
            }
            void save(checkpoint_t &cp, uint32_t pc)
            {
                cp.global_history = gshare_global_history;
                cp.local_history = local_get_bht_value(pc);
            }
            uint32_t s_state = 0;
            void s_update_prediction(uint32_t pc, bool jump)
            {
                if(jump)
                {
                    if(s_state < 3)
                    {
                        s_state++;
                    }
                }
                else
                {
                    if(s_state > 0)
                    {
                        s_state--;
                    }
                }
            }
            bool s_get_prediction(uint32_t pc)
            {
                return true;
                return s_state > 1;
            }
            bool get_prediction(uint32_t pc, uint32_t instruction, bool *jump, uint32_t *next_pc)
            {
                auto op_data = instruction;
                auto opcode = op_data & 0x7f;
                auto rd = (op_data >> 7) & 0x1f;
                auto funct3 = (op_data >> 12) & 0x07;
                auto rs1 = (op_data >> 15) & 0x1f;
                auto rs2 = (op_data >> 20) & 0x1f;
                auto funct7 = (op_data >> 25) & 0x7f;
                auto imm_i = (op_data >> 20) & 0xfff;
                auto imm_s = (((op_data >> 7) & 0x1f)) | (((op_data >> 25) & 0x7f) << 5);
                auto imm_b = (((op_data >> 8) & 0x0f) << 1) | (((op_data >> 25) & 0x3f) << 5) | (((op_data >> 7) & 0x01) << 11) | (((op_data >> 31) & 0x01) << 12);
                auto imm_u = op_data & (~0xfff);
                auto imm_j = (((op_data >> 12) & 0xff) << 12) | (((op_data >> 20) & 0x01) << 11) | (((op_data >> 21) & 0x3ff) << 1) | (((op_data >> 31) & 0x01) << 20);
                auto need_jump_prediction = true;
                auto instruction_next_pc_valid = true;
                uint32_t instruction_next_pc = 0;
                auto rd_is_link = (rd == 1) || (rd == 5);
                auto rs1_is_link = (rs1 == 1) || (rs1 == 5);
                switch(opcode)
                {
                        need_jump_prediction = false;
                        instruction_next_pc_valid = true;
                        instruction_next_pc = pc + sign_extend(imm_j, 21);
                        if(rd_is_link)
                        {
                            main_ras.push_addr(pc + 4);
                        }
                        break;
                        need_jump_prediction = false;
                        instruction_next_pc_valid = false;
                        if(rd_is_link)
                        {
                            if(rs1_is_link)
                            {
                                if(rs1 == rd)
                                {
                                    instruction_next_pc_valid = true;
                                    instruction_next_pc = call_get_prediction(pc);
                                    main_ras.push_addr(pc + 4);
                                }
                                else
                                {
                                    instruction_next_pc_valid = true;
                                    instruction_next_pc = main_ras.pop_addr();
                                    main_ras.push_addr(pc + 4);
                                }
                            }
                            else
                            {
                                instruction_next_pc_valid = true;
                                instruction_next_pc = call_get_prediction(pc);
                                main_ras.push_addr(pc + 4);
                            }
                        }
                        else
                        {
                            if(rs1_is_link)
                            {
                                instruction_next_pc_valid = true;
                                instruction_next_pc = main_ras.pop_addr();
                            }
                            else
                            {
                                instruction_next_pc_valid = true;
                                instruction_next_pc = normal_get_prediction(pc);
                            }
                        }
                        
                        break;
                        need_jump_prediction = true;
                        instruction_next_pc_valid = !cpht_get_prediction(pc) ? local_get_prediction(pc) : gshare_get_prediction(pc);
                        instruction_next_pc = instruction_next_pc_valid ? (pc + sign_extend(imm_b, 13)) : (pc + 4);
                        /*instruction_next_pc_valid = false;
                        instruction_next_pc = pc + sign_extend(imm_b, 13);*/
                        switch(funct3)
                        {
                                break;
                                return false;
                        }
                        break;
                    default:
                        return false;
                }
                if(!need_jump_prediction)
                {
                    *jump = true;
                    if(!instruction_next_pc_valid)
                    {
                        return false;
                    }
                    *next_pc = instruction_next_pc;
                }
                else
                {
                    *jump = instruction_next_pc_valid;
                    *next_pc = instruction_next_pc;
                }
                return true;
            }
            void update_prediction_guess(uint32_t pc, uint32_t instruction, bool jump, uint32_t next_pc)
            {
                auto op_data = instruction;
                auto opcode = op_data & 0x7f;
                auto rd = (op_data >> 7) & 0x1f;
                auto rs1 = (op_data >> 15) & 0x1f;
                auto rd_is_link = (rd == 1) || (rd == 5);
                auto rs1_is_link = (rs1 == 1) || (rs1 == 5);
                if(opcode == 0x63)
                {
                    gshare_global_history_update_guess(jump);
                    local_update_prediction_guess(pc, jump);
                }
            }
            void update_prediction_bru_guess(checkpoint_t &cp, uint32_t pc, uint32_t instruction, bool jump, uint32_t next_pc, bool hit)
            {
                auto op_data = instruction;
                auto opcode = op_data & 0x7f;
                auto rd = (op_data >> 7) & 0x1f;
                auto rs1 = (op_data >> 15) & 0x1f;
                auto rd_is_link = (rd == 1) || (rd == 5);
                auto rs1_is_link = (rs1 == 1) || (rs1 == 5);
                if(opcode == 0x63)
                {
                    gshare_global_history_update_bru_fix(cp, jump, hit);
                    local_update_prediction_bru_fix(cp, pc, jump, hit);
                }
            }
            void update_prediction(uint32_t pc, uint32_t instruction, bool jump, uint32_t next_pc, bool hit, int commit_index = 0)
            {
                auto op_data = instruction;
                auto opcode = op_data & 0x7f;
                auto rd = (op_data >> 7) & 0x1f;
                auto rs1 = (op_data >> 15) & 0x1f;
                auto rd_is_link = (rd == 1) || (rd == 5);
                auto rs1_is_link = (rs1 == 1) || (rs1 == 5);
                if(opcode == 0x63)
                {
                    gshare_update_prediction(pc, jump, hit);
                    local_update_prediction(pc, jump, hit);
                    cpht_update_prediction(pc, hit);
                }
                else if(opcode == 0x67)
                {
                    if(rd_is_link)
                    {
                        if(rs1_is_link)
                        {
                            if(rs1 == rd)
                            {
                                call_update_prediction(pc, jump, next_pc);
                            }
                            else
                            {
                            }
                        }
                        else
                        {
                            call_update_prediction(pc, jump, next_pc);
                        }
                    }
                    else
                    {
                        if(rs1_is_link)
                        {
                        }
                        else
                        {
                            normal_update_prediction(pc, jump, next_pc);
                        }
                    }
                }
            }
            void update_prediction_sync(uint32_t pc, uint32_t instruction, bool jump, uint32_t next_pc, bool hit)
            {
                sync_request_t t_req;
                t_req.req = sync_request_type_t::update_prediction;
                t_req.pc = pc;
                t_req.instruction = instruction;
                t_req.jump = jump;
                t_req.next_pc = next_pc;
                t_req.hit = hit;
                sync_request_q.push(t_req);
            }
            void sync()
            {
                sync_request_t t_req;
                while(!sync_request_q.empty())
                {
                    t_req = sync_request_q.front();
                    sync_request_q.pop();
                    switch(t_req.req)
                    {
                        case sync_request_type_t::update_prediction:
                            update_prediction(t_req.pc, t_req.instruction, t_req.jump, t_req.next_pc, t_req.hit);
                            break;
                    }
                }
            }
    };
}
