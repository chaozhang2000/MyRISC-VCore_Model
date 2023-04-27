#include "common.h"
#include "issue.h"

namespace pipeline
{
        
    issue::issue(component::port<readreg_issue_pack_t> *readreg_issue_port, component::fifo<issue_execute_pack_t> **issue_alu_fifo, component::fifo<issue_execute_pack_t> **issue_bru_fifo, component::fifo<issue_execute_pack_t> **issue_csr_fifo, component::fifo<issue_execute_pack_t> **issue_div_fifo, component::fifo<issue_execute_pack_t> **issue_lsu_fifo, component::fifo<issue_execute_pack_t> **issue_mul_fifo) : issue_q(component::issue_queue<issue_queue_item_t>(ISSUE_QUEUE_SIZE))
    {
        this->readreg_issue_port = readreg_issue_port;
        this->issue_alu_fifo = issue_alu_fifo;
        this->issue_bru_fifo = issue_bru_fifo;
        this->issue_csr_fifo = issue_csr_fifo;
        this->issue_div_fifo = issue_div_fifo;
        this->issue_lsu_fifo = issue_lsu_fifo;
        this->issue_mul_fifo = issue_mul_fifo;
    }
    
    issue_feedback_pack_t issue::run()
    {
        auto rev_pack = readreg_issue_port->get();
        issue_feedback_pack_t feedback_pack;

        memset(&feedback_pack, 0, sizeof(feedback_pack));
        
        //handle output
				//处理输出，主要是将发射队列中的指令进行发射
        if(!issue_q.is_empty())
        {
            issue_queue_item_t items[ISSUE_WIDTH];
            memset(&items, 0, sizeof(items));
            uint32_t id;
            
            //get up to 2 items from issue_queue
            assert(this->issue_q.get_front_id(&id));
            
						//根据发射队列的宽度，获取当前周期可能要发射的几条指令,暂存到itmes中
            for(uint32_t i = 0;i < ISSUE_WIDTH;i++)
            {
                items[i] = this->issue_q.get_item(id);
                
                if(!this->issue_q.get_next_id(id, &id))
                {
                    break;
                }
            }
            
            //generate output to execute stage
            for(uint32_t i = 0; i < ISSUE_WIDTH;i++)
            {
                if(items[i].enable)
                {
										//将需要如何可以发射，将发射的指令写入与执行部分对应的FIFO中
                    issue_execute_pack_t send_pack;
                    memset(&send_pack, 0, sizeof(send_pack));
                    
                    send_pack.enable = items[i].enable;
                    send_pack.valid = items[i].valid;
                    send_pack.rob_id = items[i].rob_id;
                    send_pack.pc = items[i].pc;
                    send_pack.imm = items[i].imm;
                    
                    send_pack.rs1 = items[i].rs1;
                    send_pack.arg1_src = items[i].arg1_src;
                    send_pack.rs1_need_map = items[i].rs1_need_map;
                    send_pack.rs1_phy = items[i].rs1_phy;
                    send_pack.src1_value = items[i].src1_value;
                    send_pack.src1_loaded = items[i].src1_loaded;
                    
                    send_pack.rs2 = items[i].rs2;
                    send_pack.arg2_src = items[i].arg2_src;
                    send_pack.rs2_need_map = items[i].rs2_need_map;
                    send_pack.rs2_phy = items[i].rs2_phy;
                    send_pack.src2_value = items[i].src2_value;
                    send_pack.src2_loaded = items[i].src2_loaded;
                    
                    send_pack.rd = items[i].rd;
                    send_pack.rd_enable = items[i].rd_enable;
                    send_pack.need_rename = items[i].need_rename;
                    send_pack.rd_phy = items[i].rd_phy;
                    
                    send_pack.csr = items[i].csr;
                    send_pack.op = items[i].op;
                    send_pack.op_unit = items[i].op_unit;
                    memcpy(&send_pack.sub_op, &items[i].sub_op, sizeof(items[i].sub_op));
                    
                    //ready to dispatch
										//index 是对应功能部件的编号，如alu若有两个则有两个编号，alu前面的fifo也有两个
										//alu_index,bru_index等都是issue的私有变量，目前都是0
                    uint32_t *unit_index = NULL;
                    uint32_t unit_cnt = 0;
                    component::fifo<issue_execute_pack_t> **unit_fifo = NULL;
                    
                    switch(send_pack.op_unit)
                    {
                        case op_unit_t::alu:
                            unit_index = &alu_index;
                            unit_cnt = ALU_UNIT_NUM;
                            unit_fifo = issue_alu_fifo;
                            break;
                            
                        case op_unit_t::bru:
                            unit_index = &bru_index;
                            unit_cnt = BRU_UNIT_NUM;
                            unit_fifo = issue_bru_fifo;
                            break;
                            
                        case op_unit_t::csr:
                            unit_index = &csr_index;
                            unit_cnt =  CSR_UNIT_NUM;
                            unit_fifo = issue_csr_fifo;
                            break;
                            
                        case op_unit_t::div:
                            unit_index = &div_index;
                            unit_cnt = DIV_UNIT_NUM;
                            unit_fifo = issue_div_fifo;
                            break;
                            
                        case op_unit_t::lsu:
                            unit_index = &lsu_index;
                            unit_cnt = LSU_UNIT_NUM;
                            unit_fifo = issue_lsu_fifo;
                            break;
                            
                        case op_unit_t::mul:
                            unit_index = &mul_index;
                            unit_cnt = MUL_UNIT_NUM;
                            unit_fifo = issue_mul_fifo;
                            break;
                    }
                    
                    //RR dispatch with full check
                    auto selected_index = *unit_index;
                    bool found = false;
                    
                    while(1)
                    {
                        if(!unit_fifo[selected_index]->is_full())
                        {
                            found = true;
                            break;
                        }
                        
												//如果当前功能单元的fifo满了，会去将数据放到下一个功能单元的fifo中，但这样的方式应该会造成当前功能单元fifo的堵塞，不太合适。
                        selected_index = (selected_index + 1) % unit_cnt;
                        if(selected_index == *unit_index)
                        {
                            break;
                        }
                    }
                    
                    if(found)
                    {
												//这里发现上面的顾虑消失了，将一条指令加入一个功能单元的fifo后，会切换下一次发射的目标fifo
                        *unit_index = (selected_index + 1) % unit_cnt;
												//将指令对应的操作推入对应的fifo,完成发射
                        assert(unit_fifo[selected_index]->push(send_pack));
												//这个是将pop的请求推进请求队列中，最后会在时钟沿统一处理所有请求，那时发射队列FIFO中的数据才会发生变化
                        this->issue_q.pop_sync();//handle ok, pop this item
                    }
                    else
                    {
                        break;
                    }
                }
            }
        }    
        
        //handle input
				//处理输入，主要是将指令存入发射队列
        if(!this->busy)
        {
            this->busy = true;
            this->last_index = 0;//from item 0
        }
        
				//finish表示能成功完成入列
        auto finish = true;
        
        for(;this->last_index < ISSUE_WIDTH;this->last_index++)
        {
						//猜测是跳过代表nop的操作
            if(!rev_pack.op_info[this->last_index].enable)
            {
                continue;
            }
            
            //if issue_queue is full, pause to handle this input until next cycle
            if(this->issue_q.is_full())
            {
                finish = false;
                break;
            }
            
            issue_queue_item_t t_item;
            memset(&t_item, 0, sizeof(t_item));
            auto cur_op = rev_pack.op_info[this->last_index];
            t_item.enable = cur_op.enable;
            t_item.valid = cur_op.valid;
            t_item.rob_id = cur_op.rob_id;
            t_item.pc = cur_op.pc;
            t_item.imm = cur_op.imm;
            
            t_item.rs1 = cur_op.rs1;
            t_item.arg1_src = cur_op.arg1_src;
            t_item.rs1_need_map = cur_op.rs1_need_map;
            t_item.rs1_phy = cur_op.rs1_phy;
            t_item.src1_value = cur_op.src1_value;
            t_item.src1_loaded = cur_op.src1_loaded;
            
            t_item.rs2 = cur_op.rs2;
            t_item.arg2_src = cur_op.arg2_src;
            t_item.rs2_need_map = cur_op.rs2_need_map;
            t_item.rs2_phy = cur_op.rs2_phy;
            t_item.src2_value = cur_op.src2_value;
            t_item.src2_loaded = cur_op.src2_loaded;
            
            t_item.rd = cur_op.rd;
            t_item.rd_enable = cur_op.rd_enable;
            t_item.op = cur_op.op;
            t_item.op_unit = cur_op.op_unit;
            memcpy(&t_item.sub_op, &cur_op.sub_op, sizeof(t_item.sub_op));
            
            issue_q.push(t_item);
        }
        
        if(finish)
        {
            this->busy = false;
            this->last_index = 0;
        }
        
				//如果发射队列繁忙，当前不能接受数据，会将stall流水线的信号发送出去
        feedback_pack.stall = this->busy;
				//这里才是更新发射队列的状态
        issue_q.sync();
        return feedback_pack;
    }
}
