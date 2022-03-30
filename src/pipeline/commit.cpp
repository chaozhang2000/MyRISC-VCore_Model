#include "common.h"
#include "commit.h"
#include "../component/csr_all.h"

namespace pipeline
{
	commit::commit(component::port<wb_commit_pack_t> *wb_commit_port, component::rat *rat, component::rob *rob, component::csrfile *csr_file, component::regfile<phy_regfile_item_t> *phy_regfile, component::checkpoint_buffer *checkpoint_buffer, component::branch_predictor *branch_predictor, component::interrupt_interface *interrupt_interface)
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
	}

    commit_feedback_pack_t commit::run()
	{
		commit_feedback_pack_t feedback_pack;
		phy_regfile_item_t default_phy_reg_item;
		bool need_flush = false;
		memset(&feedback_pack, 0, sizeof(feedback_pack));
		memset(&default_phy_reg_item, 0, sizeof(default_phy_reg_item));

		if(this->cur_state == state_t::normal)
		{
			//handle output
			if(!rob->is_empty())
			{
				assert(rob->get_front_id(&this->rob_item_id));
				feedback_pack.enable = true;
				feedback_pack.next_handle_rob_id = this->rob_item_id;
				feedback_pack.next_handle_rob_id_valid = true;
				auto first_id = this->rob_item_id;
				this->rob_item = rob->get_item(this->rob_item_id);

				if(interrupt_interface->get_cause(&interrupt_id))
				{
					interrupt_pc = this->rob_item.pc;
					assert(rob->get_tail_id(&this->restore_rob_item_id));
					feedback_pack.enable = true;
					feedback_pack.flush = true;
					cur_state = state_t::interrupt_flush;
					need_flush = true;
				}
				else
				{
					for(auto i = 0;i < COMMIT_WIDTH;i++)
					{
						this->rob_item = rob->get_item(this->rob_item_id);

						if(rob_item.finish)
						{
							feedback_pack.next_handle_rob_id_valid = rob->get_next_id(this->rob_item_id, &feedback_pack.next_handle_rob_id) && (feedback_pack.next_handle_rob_id != first_id);
							feedback_pack.committed_rob_id_valid[i] = true;
							feedback_pack.committed_rob_id[i] = this->rob_item_id;

							if(rob_item.has_exception)
							{
								assert(rob->get_tail_id(&this->restore_rob_item_id));
								feedback_pack.enable = true;
								feedback_pack.flush = true;
								cur_state = state_t::flush;
								need_flush = true;
								break;
							}
							else
							{
								rob->pop_sync();

								if(rob_item.old_phy_reg_id_valid)
								{
									rat->release_map_sync(rob_item.old_phy_reg_id);
									phy_regfile->write_sync(rob_item.old_phy_reg_id, default_phy_reg_item, false);
									rat->commit_map_sync(rob_item.new_phy_reg_id);
								}
							
								rob->set_committed(true);
								rob->add_commit_num(1);

								if(rob_item.csr_newvalue_valid)
								{
									csr_file->write_sync(rob_item.csr_addr, rob_item.csr_newvalue);
								}

								//branch handle
								if(rob_item.bru_op)
								{
									branch_num_add();

									if(rob_item.is_mret)
									{
										component::csr::mstatus mstatus;
										mstatus.load(csr_file->read_sys(CSR_MSTATUS));
										mstatus.set_mie(mstatus.get_mpie());
										csr_file->write_sys_sync(CSR_MSTATUS, mstatus.get_value());
									}

									if(rob_item.predicted)
									{
										branch_predicted_add();

										//whether prediction is success
										if((rob_item.bru_jump == rob_item.predicted_jump) && ((rob_item.bru_next_pc == rob_item.predicted_next_pc) || (!rob_item.predicted_jump)))
										{
											branch_hit_add();
											branch_predictor->update_prediction(rob_item.pc, rob_item.inst_value, rob_item.bru_jump, rob_item.bru_next_pc, true);
											checkpoint_buffer->pop_sync();
											break;
											//nothing to do
										}
										else if(rob_item.checkpoint_id_valid)
										{
											branch_miss_add();
											branch_predictor->update_prediction(rob_item.pc, rob_item.inst_value, rob_item.bru_jump, rob_item.bru_next_pc, false);
											auto cp = checkpoint_buffer->get_item(rob_item.checkpoint_id);

											if(rob_item.old_phy_reg_id_valid)
											{
												rat->cp_release_map(cp, rob_item.old_phy_reg_id);
												//phy_regfile->cp_set_data_valid(cp, rob_item.old_phy_reg_id, false);
												//rat->cp_commit_map(cp, rob_item.new_phy_reg_id);
											}

											uint32_t _cnt = 0;

											for(uint32_t i = 0;i < PHY_REG_NUM;i++)
											{
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

											rat->restore_sync(cp);
											phy_regfile->restore_sync(cp);
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
									else
									{
										feedback_pack.enable = true;
										feedback_pack.jump_enable = true;
										feedback_pack.jump = rob_item.bru_jump;
										feedback_pack.next_pc = rob_item.bru_jump ? rob_item.bru_next_pc : (rob_item.pc + 4);
										break;
									}
								}
							}
						}
						else
						{
							break;
						}

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

		return feedback_pack;
	}
}