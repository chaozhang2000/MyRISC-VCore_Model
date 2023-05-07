#include "common.h"
#include "commit.h"
#include "../component/csr_all.h"

namespace pipeline
{
	commit::commit(component::port<wb_commit_pack_t> *wb_commit_port, component::rat *rat, component::rob *rob, component::csrfile *csr_file, component::regfile<phy_regfile_item_t> *phy_regfile, component::checkpoint_buffer *checkpoint_buffer, component::branch_predictor *branch_predictor, component::interrupt_interface *interrupt_interface) : tdb(TRACE_COMMIT)
	{
		this->wb_commit_port = wb_commit_port;
		this->rat = rat;
		this->rob = rob;
		this->csr_file = csr_file;
		this->phy_regfile = phy_regfile;
		this->cur_state = state_t::normal;
		this->rob_item_id = 0;
		this->restore_rob_item_id = 0;
		this->interrupt_pc = 0;
		this->interrupt_id = riscv_interrupt_t::machine_external;
		this->checkpoint_buffer = checkpoint_buffer;
		this->branch_predictor = branch_predictor;
		this->interrupt_interface = interrupt_interface;
	}

	void commit::reset()
	{
		this->cur_state = state_t::normal;
		this->interrupt_pc = 0;
		this->interrupt_id = riscv_interrupt_t::machine_external;

		this->tdb.create(TRACE_DIR + "commit.tdb");
	
		this->tdb.write_metainfo();
        this->tdb.trace_on();
        this->tdb.capture_status();
        this->tdb.write_row();
	}

    commit_feedback_pack_t commit::run()
	{
		{
			riscv_interrupt_t t;
		}


		{
			uint32_t rob_item_id;
			auto t = rob->get_tail_id(&rob_item_id);
		}

		commit_feedback_pack_t feedback_pack;
//        bool enable;
//        bool next_handle_rob_id_valid;
//        uint32_t next_handle_rob_id;
//        bool has_exception;
//        uint32_t exception_pc;
//        bool flush;
//        uint32_t committed_rob_id[COMMIT_WIDTH];
//        bool committed_rob_id_valid[COMMIT_WIDTH];
//
//        bool jump_enable;
//        bool jump;
//        uint32_t next_pc;

		phy_regfile_item_t default_phy_reg_item; //phy_regfile_item_t是一个结构体，里面只有一个value,定义在pipeline_common.h中
		bool need_flush = false;
		memset(&feedback_pack, 0, sizeof(feedback_pack));
		memset(&default_phy_reg_item, 0, sizeof(default_phy_reg_item));
		auto origin_commit_num = rob->get_commit_num();//only for tracedb generation

		if(this->cur_state == state_t::normal)
		{
			//handle output
			if(!rob->is_empty())
			{
				assert(rob->get_front_id(&this->rob_item_id));//git_front_id是读取rob头的指针，rptr
				feedback_pack.enable = true;
				feedback_pack.next_handle_rob_id = this->rob_item_id;
				feedback_pack.next_handle_rob_id_valid = true;
				auto first_id = this->rob_item_id;
				this->rob_item = rob->get_item(this->rob_item_id);//从rob中取出头部的信息
//				rob_item中包含的信号	
//        uint32_t new_phy_reg_id;
//        uint32_t old_phy_reg_id;
//        bool old_phy_reg_id_valid;
//        bool finish;
//        uint32_t pc;
//        uint32_t inst_value;
//        bool has_exception;
//        riscv_exception_t exception_id;
//        uint32_t exception_value;
//        bool predicted;
//        bool predicted_jump;
//        uint32_t predicted_next_pc;
//        bool checkpoint_id_valid;
//        uint32_t checkpoint_id;
//        bool bru_op;
//        bool bru_jump;
//        uint32_t bru_next_pc;
//        bool is_mret;
//        uint32_t csr_addr;
//        uint32_t csr_newvalue;
//        bool csr_newvalue_valid;

				//首先看要提交时是否有中断，有的话对其进行处理中断信息不记录在rob中，异常信息记录在rob中
				if(interrupt_interface->get_cause(&interrupt_id))//interrupt_interface类有一个私有变量 *csr_file，get_cause函数从csr_file来获取发生的中断种类
				{
					interrupt_pc = this->rob_item.pc;
					assert(rob->get_tail_id(&this->restore_rob_item_id));//获取rob尾的id，但这里的restore_rob_item_id什么用暂时还不知道
					feedback_pack.enable = true;
					feedback_pack.flush = true;
					cur_state = state_t::interrupt_flush;//当前状态变为冲刷状态，怀疑有一个状态机来完成中断时的冲刷工作
					need_flush = true;
				}
				else
				{
					for(auto i = 0;i < COMMIT_WIDTH;i++)//支持几条指令同时提交
					{

						if(rob_item.finish)//rob中的finish表示当前指令可以进行提交
						{
							//get_next_id函数需要第一个参数，当前指令在rob中的位置来获取下一条指令在rob中的位置，并写入第二个参数，也就是这里的feedback_pack.next_handle_rob_id
							//下面这行会同时为next_handle_rob_id_valid，和next_handle_rob_id赋值
							feedback_pack.next_handle_rob_id_valid = rob->get_next_id(this->rob_item_id, &feedback_pack.next_handle_rob_id) && (feedback_pack.next_handle_rob_id != first_id);
							feedback_pack.committed_rob_id_valid[i] = true;
							feedback_pack.committed_rob_id[i] = this->rob_item_id;
							//--当前要提交的指令有异常则对异常进行处理
							if(rob_item.has_exception)
							{
								assert(rob->get_tail_id(&this->restore_rob_item_id));
								feedback_pack.enable = true;
								feedback_pack.flush = true;
								cur_state = state_t::flush;
								need_flush = true;
								break;
							}
							//--当前要提交的指令没有异常,进行正常提交
							else
							{
								//首先这条指令必然提交成功，可将rob进行pop
								rob->pop_sync();
								//接下来要做的就是提交阶段需要完成的任务
								//---没有异常，要提交的指令是要写寄存器的(猜测)	
								if(rob_item.old_phy_reg_id_valid)
								{
									rat->release_map_sync(rob_item.old_phy_reg_id);//会在rat的操作请求队列中push一个释放某一项的请求，在rat的sync的时候会统一处理所有请求
									phy_regfile->write_sync(rob_item.old_phy_reg_id, default_phy_reg_item, false);//同样会向物理寄存器发送一个写请求，写的值是0,清掉这个物理寄存器,而且会将对应的valid位清0表示当前寄存器内的值无效
									rat->commit_map_sync(rob_item.new_phy_reg_id);//对rat中的phy_map_table_commit对应的位进行置位，猜测是表示某个物理寄存器已经提交
									//？？？？？这里有个疑问，new_phy_reg_id和old_phy_reg_id分别是什么
								}
								//下面两行应该不重要
								rob->set_committed(true);//表示rob已经完成本次commit
								rob->add_commit_num(1);//记录commit的次数

								//---没有异常要提交的指令是要写csr的
								if(rob_item.csr_newvalue_valid)
								{
									csr_file->write_sync(rob_item.csr_addr, rob_item.csr_newvalue);//加上一条写状态寄存器的请求
								}

								//branch handle
								//---没有异常，要提交的指令是跳转指令
								if(rob_item.bru_op)//bru_op是一个bool，表示是否是跳转指令
								{
									branch_num_add();
									if(rob_item.is_mret)//mret指令被认为是属于跳转指令,下面括号中做的是对mstatus状态寄存器的写操作
									{
										component::csr::mstatus mstatus;
										mstatus.load(csr_file->read_sys(CSR_MSTATUS));
										mstatus.set_mie(mstatus.get_mpie());
										csr_file->write_sys_sync(CSR_MSTATUS, mstatus.get_value());
									}
									//----没有异常，要提交的指令是跳转指令，且做过预测
									if(rob_item.predicted)
									{
										branch_predicted_add();
										//whether prediction is success bru_jump代表是b指令（猜测）
										//-----没有异常，要提交的是跳转，做过预测，预测正确   预测成功的判断标准是，预测是否跳转和真实是否跳转的情况相同，若预测为跳转则还要额外要求跳转地址和预测跳转地址相同
										if((rob_item.bru_jump == rob_item.predicted_jump) && ((rob_item.bru_next_pc == rob_item.predicted_next_pc) || (!rob_item.predicted_jump)))
										{
											branch_hit_add();
											branch_predictor->update_prediction(rob_item.pc, rob_item.inst_value, rob_item.bru_jump, rob_item.bru_next_pc, true, i);
											checkpoint_buffer->pop_sync();
											break;
											//nothing to do
										}
										//-----没有异常，要提交的是跳转，做过预测，预测失败
										else if(rob_item.checkpoint_id_valid)
										{
											branch_miss_add();
											branch_predictor->update_prediction(rob_item.pc, rob_item.inst_value, rob_item.bru_jump, rob_item.bru_next_pc, false, i);//对分支预测器的操作
											auto cp = checkpoint_buffer->get_item(rob_item.checkpoint_id);//从checkpoint_buffer获取需要恢复的数据。
											if(rob_item.old_phy_reg_id_valid)
											{
												rat->cp_release_map(cp, rob_item.old_phy_reg_id); //这里实际上就是cp_set_vaild(cp,old_phy_reg_id,false),在rat中将对应的编号的valid取消来进行释放。但old_physical_reg_id是什么还不知道？？？
												//phy_regfile->cp_set_data_valid(cp, rob_item.old_phy_reg_id, false);
												//rat->cp_commit_map(cp, rob_item.new_phy_reg_id);
											}

											uint32_t _cnt = 0;

											//下面对物理寄存器和rat的valid进行处理，将cp中显示visible的在rat和phy_regfile中对应的valid位置位
											for(uint32_t i = 0;i < PHY_REG_NUM;i++)
											{
												//下面这段代码中的cp_get_visible和cp_set_visible实际都是对checkpoint的操作，并不是在对rat和物理寄存器进行恢复，下面这段代码作的是将valid位按照visible来设置
												if(!rat->cp_get_visible(cp, i))
												{
													rat->cp_set_valid(cp, i, false);
													phy_regfile->cp_set_data_valid(cp, i, false);
												}
												else
												{
													//assert(phy_regfile->cp_get_data_valid(cp, i));
													phy_regfile->cp_set_data_valid(cp, i, true);
													_cnt++;
												}
											}

											assert(_cnt == 31);

											rat->restore_sync(cp);//将restore请求压入请求队列，在下个周期就会实现恢复，但这里的恢复只是根据cp中的rat_phy_map_table_valid,和rat_phy_map_table_visible来恢复rat中的对应结构，只是恢复了valid和visible就够吗，还没发现恢复数据的地方？？？？？
											phy_regfile->restore_sync(cp);//与上一行相同，只是对物理寄存器操作，也是拷贝valid位。
											//对feedback_pack的信号赋值，jump代表是否是条件跳转，next_pc来自rob
											feedback_pack.enable = true;
											feedback_pack.flush = true;
											feedback_pack.jump_enable = true;
											feedback_pack.jump = rob_item.bru_jump;
											feedback_pack.next_pc = rob_item.bru_jump ? rob_item.bru_next_pc : (rob_item.pc + 4);
											rob->flush();
											checkpoint_buffer->flush();
											need_flush = true;
											break;
										}
										else
										{
											//it's not possible
											assert(false);
										}
									}
									//----没有异常，要提交的指令是跳转指令，但没有做过预测(猜测) !rob_item.predicted （我现在认为这种情况也不会发生）
									else
									{
										feedback_pack.enable = true;
										feedback_pack.jump_enable = true;
										feedback_pack.jump = rob_item.bru_jump;
										feedback_pack.next_pc = rob_item.bru_jump ? rob_item.bru_next_pc : (rob_item.pc + 4);
										break;
									}
								//---
								}
							//--
							}
						//-
						}
						//rob中的当前指令还没有finish，不能提交，这个周期的commit什么也不做
						else
						{
							break;
						}

						//在尝试进行commit rob中的下一条指令的时候，遇到下面情况则停止本周期的commit
						//rob中取不到下一条指令了或者，或者下条还没有valid，这里rob_item_id==first_id就表示下一条没有valid,可见feedback_pack.next_handle_rob_id_valid信号的赋值
						if(!rob->get_next_id(this->rob_item_id, &this->rob_item_id) || (this->rob_item_id == first_id))
						{
							break;
						}
					}
				}
			}

			//handle input
			if(!need_flush)
			{
				auto rev_pack = wb_commit_port->get();

				for(auto i = 0;i < EXECUTE_UNIT_NUM;i++)
				{

					if(rev_pack.op_info[i].enable)
					{
						auto rob_item = rob->get_item(rev_pack.op_info[i].rob_id);
						rob_item.finish = true;
						rob_item.has_exception = rev_pack.op_info[i].has_exception;
						rob_item.exception_id = rev_pack.op_info[i].exception_id;
						rob_item.exception_value = rev_pack.op_info[i].exception_value;
						rob_item.predicted = rev_pack.op_info[i].predicted;
						rob_item.predicted_jump = rev_pack.op_info[i].predicted_jump;
						rob_item.predicted_next_pc = rev_pack.op_info[i].predicted_next_pc;
						rob_item.checkpoint_id_valid = rev_pack.op_info[i].checkpoint_id_valid;
						rob_item.checkpoint_id = rev_pack.op_info[i].checkpoint_id;
						rob_item.bru_op = rev_pack.op_info[i].op_unit == op_unit_t::bru;
						rob_item.bru_jump = rev_pack.op_info[i].bru_jump;
						rob_item.bru_next_pc = rev_pack.op_info[i].bru_next_pc;
						rob_item.csr_newvalue = rev_pack.op_info[i].csr_newvalue;
						rob_item.csr_newvalue_valid = rev_pack.op_info[i].csr_newvalue_valid;
						rob->set_item_sync(rev_pack.op_info[i].rob_id, rob_item);
					}
				}
			}

		}
		else if(this->cur_state == state_t::flush)//flush
		{
			//flush rob and restore rat
			auto t_rob_item = rob->get_item(this->restore_rob_item_id);
			if(t_rob_item.old_phy_reg_id_valid)
			{
				rat->restore_map_sync(t_rob_item.new_phy_reg_id, t_rob_item.old_phy_reg_id);
				phy_regfile->write_sync(t_rob_item.new_phy_reg_id, default_phy_reg_item, false);
			}

			{
				uint32_t id;
				auto t = rob->get_prev_id(this->restore_rob_item_id, &id);
				this->tdb.update_signal<uint8_t>(trace::domain_t::input, "rob_commit_flush_next_id", id, 0);
				this->tdb.update_signal<uint8_t>(trace::domain_t::input, "rob_commit_flush_next_id_valid", t, 0);
			}

			if((this->restore_rob_item_id != this->rob_item_id) && rob->get_prev_id(this->restore_rob_item_id, &this->restore_rob_item_id))
			{
				feedback_pack.enable = true;
				feedback_pack.flush = true;
			}
			else
			{
				rob->flush();
				feedback_pack.enable = true;
				feedback_pack.has_exception = true;
				csr_file->write_sys_sync(CSR_MEPC, rob_item.pc);
				csr_file->write_sys_sync(CSR_MTVAL, rob_item.exception_value);
				csr_file->write_sys_sync(CSR_MCAUSE, static_cast<uint32_t>(rob_item.exception_id));
				feedback_pack.exception_pc = csr_file->read_sys(CSR_MTVEC);
				feedback_pack.flush = true;
				cur_state = state_t::normal;
				rob->set_committed(true);
				rob->add_commit_num(1);
			}
		}
		else if(this->cur_state == state_t::interrupt_flush)//interrupt_flush
		{
			//flush rob and restore rat
			auto t_rob_item = rob->get_item(this->restore_rob_item_id);
			
			if(t_rob_item.old_phy_reg_id_valid)
			{
				rat->restore_map_sync(t_rob_item.new_phy_reg_id, t_rob_item.old_phy_reg_id);
				phy_regfile->write_sync(t_rob_item.new_phy_reg_id, default_phy_reg_item, false);
			}

			if((this->restore_rob_item_id != this->rob_item_id) && rob->get_prev_id(this->restore_rob_item_id, &this->restore_rob_item_id))
			{
				feedback_pack.enable = true;
				feedback_pack.flush = true;
			}
			else
			{
				rob->flush();
				feedback_pack.enable = true;
				feedback_pack.has_exception = true;
				csr_file->write_sys_sync(CSR_MEPC, interrupt_pc);
				csr_file->write_sys_sync(CSR_MTVAL, 0);
				csr_file->write_sys_sync(CSR_MCAUSE, 0x80000000 | static_cast<uint32_t>(interrupt_id));
				component::csr::mstatus mstatus;
				mstatus.load(csr_file->read_sys(CSR_MSTATUS));
				mstatus.set_mpie(mstatus.get_mie());
				mstatus.set_mie(false);
				csr_file->write_sys_sync(CSR_MSTATUS, mstatus.get_value());
				interrupt_interface->set_ack_sync(interrupt_id);
				feedback_pack.exception_pc = csr_file->read_sys(CSR_MTVEC);
				feedback_pack.flush = true;
				cur_state = state_t::normal;
				rob->set_committed(true);
				rob->add_commit_num(1);
			}
		}
		
        
        for(auto i = 0;i < COMMIT_WIDTH;i++)
        {
        }
		

		this->tdb.capture_output_status();
        this->tdb.write_row();
		return feedback_pack;
	}
}
