#include "Graphics/RHI/Platforms/Vulkan/PassTimestampTicket.h"
#include <cstdio>
#include <bit>
using primal::graphics::rhi::FrameSlotCompletion;
using primal::graphics::rhi::pass_timestamp::Tickets;
int main(){int n=0,f=0;auto c=[&](bool b,const char* s){++n;if(!b){++f;std::fprintf(stderr,"FAIL %s\n",s);}};FrameSlotCompletion fence;Tickets t;
 c(!t.OnFence(fence,0,4),"unpublished fence");fence.Publish(0);c(t.OnFence(fence,0,4),"initial completion");c(t.Get(0)->status==Tickets::Status::Free,"initial fence has no result");
 c(t.Start(0,2,4,128,1),"record start");c(!t.Start(0,2,4,128,2),"multiple graph cannot reset");c(!t.OnFence(fence,0,4),"no repeated serial");
 c(!t.Commit(0,2,4,128,3,true,true),"wrong ticket");c(t.Get(0)->status==Tickets::Status::Quarantine,"mismatch quarantined");fence.Invalidate();fence.Publish(0);t.OnFence(fence,0,4);c(t.Get(0)->status==Tickets::Status::Quarantine,"later serial cannot redeem unknown submission");
 fence.Invalidate();fence.Publish(1);c(t.OnFence(fence,1,5),"other slot");c(t.Start(1,8,5,144,3),"slot1 start");c(t.Commit(1,8,5,144,3,true,true),"exact submitted");c(!t.Retire(1,t.Get(1)->generation),"not complete cannot retire");
 fence.Invalidate();fence.Publish(2);c(!t.OnFence(fence,1,5),"wrong active slot");fence.Invalidate();fence.Publish(1);c(t.OnFence(fence,1,5),"fence completion");auto g=t.Get(1)->generation;c(t.Get(1)->status==Tickets::Status::Ready,"completed ready");c(!t.Retire(1,g+1),"wrong generation rejected");c(t.Retire(1,g),"retire once");c(!t.Retire(1,g),"double retire rejected");c(t.Start(1,9,5,240,4),"new generation after frame jump");c(t.Get(1)->generation==g+1,"generation increases");c(!t.Commit(1,9,5,240,4,false,true),"failed end quarantines");
 using primal::graphics::rhi::pass_timestamp::BoundTickets;
 {
  BoundTickets owned;FrameSlotCompletion first;
  c(owned.binding.BindOrMatch(0x1000,1),"bind actual device lifetime");first.Publish(0);
  c(owned.tickets.OnFence(first,0,9)&&owned.tickets.Start(0,7,9,128,1)&&owned.tickets.Commit(0,7,9,128,1,true,true),"old device pending");
  c(!owned.binding.BindOrMatch(0x2000,2),"different device rejected before fence observation");
  c(owned.tickets.Get(0)->status==Tickets::Status::Pending,"old ticket remains isolated");
  c(!owned.Release(0x1000,1,false),"failed idle cannot release");
  c(!owned.Release(0x2000,2,true),"new device cannot release old pool");
  c(!owned.binding.BindOrMatch(0x1000,1),"mismatch locks old owner until safe release");
  c(owned.Release(0x1000,1,true),"old successful idle releases");
  c(owned.tickets.Get(0)->status==Tickets::Status::Free && owned.tickets.Serial(0)==0,"release resets ticket and serial");
  c(owned.binding.BindOrMatch(0x1000,2),"same address new generation may bind after release");
  c(!owned.binding.BindOrMatch(0x1000,3),"same address changed generation rejected");
  c(!owned.Release(0x1000,3,true),"reused address cannot release prior generation");
  c(owned.Release(0x1000,2,true),"original generation safely releases");
  c(owned.binding.BindOrMatch(0x2000,3),"new device binds after cleanup");
  c(!owned.binding.BindOrMatch(0x2000,0),"unknown generation rejected");
 }
 using primal::graphics::rhi::pass_timestamp::Evaluate;
 c(!Evaluate(-4,1,1,1,2,64,1,100).accepted,"injected device error rejected");
 c(!Evaluate(0,1,0,1,2,64,1,100).accepted,"partial availability rejected");
 c(!Evaluate(0,1,1,1,2,0,1,100).accepted,"unsupported valid bits rejected");
 c(!Evaluate(0,1,1,250,3,8,1,100).accepted,"wrap rejected");
 c(!Evaluate(0,1,1,1,2,8,1,256).accepted,"host bound admits full wrap");
 c(!Evaluate(0,1,1,1,10,64,1,4).accepted,"duration exceeds host bound");
 const auto nan=std::bit_cast<double>(uint64_t(0x7ff8000000000000ull));
 const auto inf=std::bit_cast<double>(uint64_t(0x7ff0000000000000ull));
 c(!Evaluate(0,1,1,1,2,64,nan,100).accepted && !Evaluate(0,1,1,1,2,64,inf,100).accepted,"nonfinite period under fast math rejected");
 c(!Evaluate(0,1,1,1,2,64,1,inf).accepted,"nonfinite host bound rejected");
 auto good=Evaluate(0,1,1,10,20,64,2,100);c(good.accepted&&good.nanoseconds==20,"fully qualified duration");
 std::printf("%d/%d checks passed\n",n-f,n);return f?1:0;}
