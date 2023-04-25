#pragma once
#include "common.h"
#include "config.h"
#include "../component/port.h"
#include "../component/fifo.h"
#include "../component/issue_queue.h"
#include "readreg_issue.h"
#include "issue_execute.h"

//这是代表流水线中的发射阶段
//主要是两部分
//第一部分是run()，就是这阶段的运行
//第二部分是支持这部分运行的数据结构
//	1.输入和输出的接口信号的指针，因为发射队列的数量不只一个，发射的目的地也不只一个，所以都是指针的指针
//	2.发射队列issue_queue 里面实际上是包含了真实存放发射队列中数据的FIFO,存放的数据是这里定义的issue_queue_item_t,以及一些模拟真实发射队列行为的数据结构和函数方法。
namespace pipeline
{
    typedef struct issue_feedback_pack_t
    {
        bool stall;
    }issue_feedback_pack_t;
    
    class issue
    {
        private:
            typedef struct issue_queue_item_t
            {
                bool enable;//this item has op
                bool valid;//this item has valid op
                uint32_t rob_id;
                uint32_t pc;
                uint32_t imm;

                uint32_t rs1;
                arg_src_t arg1_src;
                bool rs1_need_map;
                uint32_t rs1_phy;
                uint32_t src1_value;
                bool src1_loaded;

                uint32_t rs2;
                arg_src_t arg2_src;
                bool rs2_need_map;
                uint32_t rs2_phy;
                uint32_t src2_value;
                bool src2_loaded;

                uint32_t rd;
                bool rd_enable;
                bool need_rename;
                uint32_t rd_phy;

                uint32_t csr;
                op_t op;
                op_unit_t op_unit;
                
                union
                {
                    alu_op_t alu_op;
                    bru_op_t bru_op;
                    div_op_t div_op;
                    lsu_op_t lsu_op;
                    mul_op_t mul_op;
                    csr_op_t csr_op;
                }sub_op;
            }issue_queue_item_t;
            
            component::port<readreg_issue_pack_t> *readreg_issue_port;
            component::fifo<issue_execute_pack_t> **issue_alu_fifo;
            component::fifo<issue_execute_pack_t> **issue_bru_fifo;
            component::fifo<issue_execute_pack_t> **issue_csr_fifo;
            component::fifo<issue_execute_pack_t> **issue_div_fifo;
            component::fifo<issue_execute_pack_t> **issue_lsu_fifo;
            component::fifo<issue_execute_pack_t> **issue_mul_fifo;
            
            component::issue_queue<issue_queue_item_t> issue_q;
            bool busy = false;
            uint32_t last_index = 0;
            uint32_t alu_index = 0;
            uint32_t bru_index = 0;
            uint32_t csr_index = 0;
            uint32_t div_index = 0;
            uint32_t lsu_index = 0;
            uint32_t mul_index = 0;
               
            public:
            issue(component::port<readreg_issue_pack_t> *readreg_issue_port, component::fifo<issue_execute_pack_t> **issue_alu_fifo, component::fifo<issue_execute_pack_t> **issue_bru_fifo, component::fifo<issue_execute_pack_t> **issue_csr_fifo, component::fifo<issue_execute_pack_t> **issue_div_fifo, component::fifo<issue_execute_pack_t> **issue_lsu_fifo, component::fifo<issue_execute_pack_t> **issue_mul_fifo);
            issue_feedback_pack_t run();
    };
}
