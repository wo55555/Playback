#include "WorldActorCommands.h"
#include "playback/editor/editing/models/EditorStateExt.h"
#include "playback/editor/editing/WorldActorOps.h"
namespace playback::editor::editing::command {
SplitWorldActorAtPlayhead::SplitWorldActorAtPlayhead(int tick):mTick(tick){} void SplitWorldActorAtPlayhead::execute(model::EditorStateExt&s){mBefore=s;WorldActorOps::splitAt(s.worldActor,mTick);} void SplitWorldActorAtPlayhead::undo(model::EditorStateExt&s){if(mBefore)s=*mBefore;} std::string SplitWorldActorAtPlayhead::label()const{return "Split World Actor";}
TrimWorldActorSegment::TrimWorldActorSegment(std::string id,int start,int end):mId(std::move(id)),mStart(start),mEnd(end){} void TrimWorldActorSegment::execute(model::EditorStateExt&s){mBefore=s;WorldActorOps::trimSegment(s.worldActor,mId,mStart,mEnd,s.totalTicks);} void TrimWorldActorSegment::undo(model::EditorStateExt&s){if(mBefore)s=*mBefore;} std::string TrimWorldActorSegment::label()const{return "Trim World Actor";}
SetWorldActorSegmentSpeed::SetWorldActorSegmentSpeed(std::string id,float speed):mId(std::move(id)),mSpeed(speed){} void SetWorldActorSegmentSpeed::execute(model::EditorStateExt&s){mBefore=s;WorldActorOps::setSpeed(s.worldActor,mId,mSpeed);} void SetWorldActorSegmentSpeed::undo(model::EditorStateExt&s){if(mBefore)s=*mBefore;} std::string SetWorldActorSegmentSpeed::label()const{return "Set World Actor Speed";}
RippleDeleteWorldActorSeg::RippleDeleteWorldActorSeg(std::string id):mId(std::move(id)){} void RippleDeleteWorldActorSeg::execute(model::EditorStateExt&s){mBefore=s;WorldActorOps::rippleDelete(s.worldActor,mId,s.totalTicks);} void RippleDeleteWorldActorSeg::undo(model::EditorStateExt&s){if(mBefore)s=*mBefore;} std::string RippleDeleteWorldActorSeg::label()const{return "Ripple Delete World Actor";}
}
