class WorldFramePresentation {
public:
    bool requested=false,enabled=false,failed=false,visible=false,producing=false;
    uint64_t epoch=0,device=0;
    double elapsed=0;
    std::string error;
    GxWorld::WorldFrame displayed;
    GxWorld::WorldFrameLease action;
    std::array<std::shared_ptr<const WorldInteractionFrame>,GxWorld::CompletedWorld::SlotCount> snapshots;
    std::shared_ptr<const WorldInteractionFrame> interaction;
    std::unique_ptr<WorldInteractionFrame::State> input;
    void Clear(){visible=false;interaction.reset();input.reset();snapshots={};displayed={};action={};elapsed=0;}
    void Poll(CGxDevice& gx){
        GxWorld::WorldFrame next;visible=false;
        if(!gx.SceneComposeCompletedWorld(epoch,&next))return;
        auto snapshot=snapshots[next.lease.slot];
        if(!snapshot || !snapshot->OwnerMatches(next))throw std::logic_error("Completed world has no image-matched interaction snapshot");
        if(!interaction || !(displayed.lease==next.lease)){
            interaction=std::move(snapshot);input=std::make_unique<WorldInteractionFrame::State>(interaction->BeginInteraction());
        }
        displayed=next;visible=true;
    }
};
bool CGWorldFrame::HasDisplayedWorld() const {
    return m_presentation && m_presentation->enabled && m_presentation->visible && !m_presentation->failed &&
        !LoadingScreenDrawing() && m_presentation->interaction && m_presentation->interaction->OwnerMatches(m_presentation->displayed);
}
bool CGWorldFrame::DisplayedWorld(GxWorld::WorldFrame& frame) const{
    if(!HasDisplayedWorld())return false;frame=m_presentation->displayed;return true;
}
bool WorldInteractionFrame::OwnerMatches(const Frame& frame) const {
    return g_theGxDevicePtr && MatchesOwnerVersion(frame,WorldDisplayEpoch(),g_theGxDevicePtr->ScenePresentationVersion());
}
std::vector<WorldInteractionFrame::ActorPolicy> WorldInteractionFrame::ActorPolicies(CGWorldFrame& owner,const Frame& frame,const State& state,uint32_t flags) const {
    if(!OwnerMatches(frame) || state.owner!=this)throw std::logic_error("Displayed actor policy has expired");
    std::vector<ActorPolicy> result;if(!(flags&0x7c))return result;
    const auto time=OsGetAsyncTimeMs();const bool terrain=Spell_C_TargetingSpellAllowsTerrain()!=0;
    for(auto root:state.order){
        const WOWGUID guid{ActorGuid(root)};auto* object=ClntObjMgrObjectPtr(guid,TYPEMASK_OBJECT,__FILE__,__LINE__);
        if(!object || !owner.IsLegalSelection(object,flags) || (!object->CanHighlight()&&!terrain))continue;
        const bool low=(object->m_obj->m_type&TYPEMASK_UNIT) && static_cast<CGUnit_C*>(object)->IsLowPrioritySelection(time);
        result.push_back({root,guid==owner.m_hitGuid?UINT32_MAX:uint32_t(GetObjectHitTestType(object)),low});
    }
    return result;
}
WorldInteractionFrame::Hit WorldInteractionFrame::PickOnOwner(CGWorldFrame& owner,const Frame& frame,State& state,const C3Vector& start,const C3Vector& end,uint32_t flags) const {
    if(!OwnerMatches(frame) || !World::s_m2Scene)throw std::logic_error("Displayed world interaction has expired");
    auto hit=Pick(*World::s_m2Scene,frame,state,start,end,flags,ActorPolicies(owner,frame,state,flags));
    if(hit.kind==3){
        auto* object=ClntObjMgrObjectPtr(WOWGUID{hit.guid},TYPEMASK_OBJECT,__FILE__,__LINE__);
        hit=ResolveCollision(frame,hit,!object?CollisionObject::Missing:object->IsTransport()?CollisionObject::Transport:CollisionObject::Ordinary);
    }
    if(hit.hasDepth)g_theGxDevicePtr->CursorSetDepth(hit.depth);
    return hit;
}
int32_t WorldInteractionFrame::HitTestPointOnOwner(CGWorldFrame& owner,const Frame& frame,State& state,float x,float y,int32_t filter,HITTESTRESULT* result) const {
    if(!OwnerMatches(frame) || !result) return 0;
    const auto* input=CGInputControl::GetActive();if(!input || !(input->m_mouseModeFlags&1))return 0;
    const auto flags=owner.GetHitTestFilterFlags(filter);if(!flags)return 0;
    C3Vector start,end;if(!LineSegment(frame,x,y,start,end))return 0;
    result->m_segStart=start;result->m_segEnd=end;
    const auto hit=PickOnOwner(owner,frame,state,start,end,flags);
    if(hit.kind){result->m_guid=WOWGUID{hit.guid};result->m_point=hit.point;result->m_distance=hit.distance;}
    if(hit.kind>=2)owner.m_hitGuid=result->m_guid;
    else {owner.MoveToFreeList(&owner.m_hitModelList);owner.m_hitGuid=WOWGUID();}
    return hit.kind;
}
int32_t CGWorldFrame::HitDisplayedWorld(const C3Vector& start,const C3Vector& end,uint32_t flags,HITTESTRESULT* result) {
    if(!HasDisplayedWorld() || !result)return 0;
    auto& s=*m_presentation;const auto hit=s.interaction->PickOnOwner(*this,s.displayed,*s.input,start,end,flags);
    if(hit.kind){result->m_guid=WOWGUID{hit.guid};result->m_point=hit.point;result->m_distance=hit.distance;}
    return hit.kind;
}
std::shared_ptr<const WorldInteractionFrame> CGWorldFrame::CaptureWorldInteraction(GxWorld::WorldFrameLease lease) {
    const auto epoch=WorldDisplayEpoch(),version=g_theGxDevicePtr->ScenePresentationVersion();
    if(!m_camera || !World::s_m2Scene)throw std::logic_error("Missing world interaction source");
    std::vector<M2HitSnapshotRoot> roots;
    for(auto* record=m_modelList.Head();record;record=m_modelList.Next(record))
        if(record->m_model && record->m_guid.m_guid)roots.push_back({record->m_model,record->m_guid.m_guid});
    auto actors=std::make_shared<const WorldModelPickFrame>(lease,m_camera->m_position,m_viewportDDC,M2HitSceneSnapshot::Capture(*World::s_m2Scene,roots));
    auto collision=WorldCollisionFrame::Capture();
    WorldInteractionFrame::Screen screen;screen.nearZ=m_camera->m_nearZ;screen.viewportNDC=m_viewportNDC;screen.rect=m_rect;
    NDCToDDC(1,1,&screen.ddcWidth,&screen.ddcHeight);
    std::unordered_map<uint64_t,C44Matrix> transports;
    for(auto guid:collision->CollisionGuids()){
        auto* object=ClntObjMgrObjectPtr(WOWGUID{guid},TYPEMASK_OBJECT,__FILE__,__LINE__);
        if(object && object->IsTransport())transports.emplace(guid,object->GetMatrix().AffineInverse());
    }
    if(epoch!=WorldDisplayEpoch() || version!=g_theGxDevicePtr->ScenePresentationVersion())throw std::logic_error("World changed during interaction capture");
    return std::make_shared<const WorldInteractionFrame>(std::move(actors),std::move(collision),screen,std::move(transports),epoch,version);
}
