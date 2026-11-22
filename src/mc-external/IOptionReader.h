#include "mc/deviceinfo/DeviceMemoryTier.h"
#include "mc/options/GraphicsMode.h"
#include "mc/options/UIProfile.h"
#include "mc/options/option_types/Option.h"
#include "mc/options/option_types/OptionID.h"

class IOptionsReader {
public:
    virtual ~IOptionsReader();
    virtual gsl::not_null<const Option*>    get(OptionID) const;
    virtual bool                            getDevRenderBoundingBoxes() const;
    virtual bool                            getDevRenderPaths() const;
    virtual bool                            getDevRenderMobInfoState() const;
    virtual bool                            getDevRenderSchedulerInfo() const;
    virtual bool                            getDevRenderGoalState() const;
    virtual bool                            getDevDeepDarkDebugRender() const;
    virtual uint16_t                        getDevGameEventRetentionTicks() const;
    virtual std::optional<DeviceMemoryTier> getScriptingMemoryTierOverride() const;
    virtual std::optional<int>              getDebugTextFilteringDelayMilliSeconds() const;
    virtual bool                            getFilterProfanity() const;
    virtual int                             getMaxViewDistanceChunks() const;
    virtual GraphicsMode                    getGraphicsMode() const;
    virtual UIProfile                       getUIProfile() const;
};
