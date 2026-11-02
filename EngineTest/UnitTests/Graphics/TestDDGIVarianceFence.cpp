#include "Graphics/Lumen/DDGI/VarianceFenceTicket.h"
#include <cstdio>
using primal::graphics::lumen::VarianceFenceTicket;
using primal::graphics::rhi::FrameSlotCompletion;
int main(){int tests=0,fail=0;auto check=[&](bool ok,const char* label){++tests;if(!ok){++fail;std::fprintf(stderr,"FAIL %s\n",label);}};
 FrameSlotCompletion c;VarianceFenceTicket t,other;constexpr auto invalid=VarianceFenceTicket::Invalid;
 check(!t.Begin(c,0,1,2,0,1),"no completion proof");c.Publish(0);
 check(t.Begin(c,0,1,2,0,1),"initial successful begin");check(!t.Take(10,64),"initial signaled fence proves no copy");
 check(t.Record(10,64,20,100),"record copy");check(!t.Record(10,64,20,100),"duplicate record rejected");
 check(!t.Commit(0,99,2,0,1,true),"wrong command rejected");check(!t.Commit(0,1,2,0,1,true),"mismatched commit revokes ticket");
 check(!t.Begin(c,0,3,2,3,3),"same serial cannot authenticate");
 c.Invalidate();c.Publish(1);check(!t.Begin(c,0,3,2,3,3),"wrong active slot");c.Invalidate();c.Publish(0);
 check(t.Begin(c,0,1,2,0,2)&&t.Record(10,64,20,100)&&t.Commit(0,1,2,0,2,true),"exact successful submission");
 check(!t.Commit(0,1,2,0,1,true),"duplicate submit rejected");c.Invalidate();c.Publish(0);
 check(t.Begin(c,0,3,2,3,3),"slot fence completed");auto* e=t.Take(10,64);check(e&&e->source==20&&e->source_frame==100&&e->context.frame==0,"matching submitted ticket and source");
 check(!t.Take(10,64),"single read attempt");check(t.Record(10,64,21,103),"new copy after reuse");check(!t.Commit(0,3,2,3,3,false),"failed submit revoked");
 c.Invalidate();c.Publish(0);check(t.Begin(c,0,4,2,6,4),"completion after failed submit");check(!t.Take(10,64),"failed copy never certified by later fence");
 check(t.Record(10,64,21,106),"discarded recording setup");c.Invalidate();c.Publish(0);check(t.Begin(c,0,5,2,9,5),"new begin discards unsubmitted recording");check(!t.Take(10,64),"discarded copy stays invalid");
 check(t.Record(10,64,20,109)&&t.Commit(0,5,2,9,5,true),"new ticket committed");c.Invalidate();c.Publish(0);check(t.Begin(c,0,6,2,12,6),"new completion");check(!t.Take(11,64)&&!t.Take(10,64),"changed buffer invalidates ticket for this frame");check(!t.Commit(0,6,8,12,6,true),"wrong fence refused");t.Reset();check(!t.Commit(0,6,2,12,6,true),"reset rejects old commit");
 c.Invalidate();c.Publish(0);check(t.Begin(c,0,7,2,1000,7)&&!t.Take(10,64),"reset and frame jump retain no copy");check(!t.Record(invalid,64,20,1)&&!t.Record(10,0,20,1),"invalid resource and empty range");
 check(t.Record(10,64,20,1000),"record before ticket replay");
 check(!t.Commit(0,7,2,1000,6,true)&&!t.Commit(0,7,2,1000,7,true),"old ticket invalidates new recording");
 check(other.Begin(c,0,55,2,1000,7)&&!other.Take(10,64),"separate volume has no other ticket");
 {
    using primal::graphics::lumen::VarianceSceneIdentity;
    using primal::graphics::lumen::ReconcileVarianceScene;
    struct Probe {float max_depth_variance;};std::array<Probe,2> p{{{2.f},{3.f}}};
    VarianceSceneIdentity identity;VarianceFenceTicket tickets;bool confirmed=true;
    ReconcileVarianceScene(identity,tickets,confirmed,p,4,100);
    check(identity.Available() && confirmed && p[0].max_depth_variance==2.f,"initial known identity");
    ReconcileVarianceScene(identity,tickets,confirmed,p,4,0);
    check(!identity.Available() && confirmed && p[0].max_depth_variance==2.f,"same epoch unknown digest suspends and retains confirmed");
    ReconcileVarianceScene(identity,tickets,confirmed,p,4,100);
    check(identity.Available() && confirmed && p[0].max_depth_variance==2.f,"same identity restores confirmed");
    ReconcileVarianceScene(identity,tickets,confirmed,p,5,0);
    check(!identity.Available() && !confirmed && p[0].max_depth_variance==0.f && p[1].max_depth_variance==0.f,"changed epoch unknown digest invalidates priority values");
    ReconcileVarianceScene(identity,tickets,confirmed,p,5,200);
    check(identity.Available() && !confirmed && p[0].max_depth_variance==0.f,"new identity does not restore prior values");
    confirmed=true;p[0].max_depth_variance=4.f;
    ReconcileVarianceScene(identity,tickets,confirmed,p,0,0);
    check(!identity.Available() && !confirmed && p[0].max_depth_variance==0.f,"unknown epoch clears cross-scene values");
    ReconcileVarianceScene(identity,tickets,confirmed,p,5,200);
    check(identity.Available() && !confirmed && p[0].max_depth_variance==0.f,"recovery after unknown epoch remains unconfirmed");
    confirmed=true;p[0].max_depth_variance=6.f;
    ReconcileVarianceScene(identity,tickets,confirmed,p,5,0);
    ReconcileVarianceScene(identity,tickets,confirmed,p,5,201);
    check(identity.Available() && !confirmed && p[0].max_depth_variance==0.f,"same epoch changed digest after unknown invalidates confirmed");
    confirmed=true;p[0].max_depth_variance=7.f;
    ReconcileVarianceScene(identity,tickets,confirmed,p,6,0);
    check(!identity.Available() && !confirmed && p[0].max_depth_variance==0.f,"epoch transition invalidates even without digest");
 }
 std::printf("%d/%d checks passed\n",tests-fail,tests);return fail?1:0;}
