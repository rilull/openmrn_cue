
#ifndef _BRACZ_CS_TIVA_CONFIG_HXX_
#define _BRACZ_CS_TIVA_CONFIG_HXX_

#include "nmranet/ConfigRepresentation.hxx"
#include "nmranet/MemoryConfig.hxx"
#include "nmranet/SimpleNodeInfo.hxx"
#include "commandstation/TrainDbCdi.hxx"

namespace nmranet {

extern const SimpleNodeStaticValues SNIP_STATIC_DATA =
{
    4, "Balazs Racz", "CS.Tiva", "v2015.01.02", "1.0.2"
};

static constexpr uint16_t CANONICAL_VERSION = 0xa732;

CDI_GROUP(BaseSegment, Segment(MemoryConfigDefs::SPACE_CONFIG), Offset(128));
CDI_GROUP_ENTRY(internal_config, InternalConfigData);
CDI_GROUP_END();

CDI_GROUP(TrainSegment, Segment(MemoryConfigDefs::SPACE_CONFIG), Offset(1024));
CDI_GROUP_ENTRY(all_trains, commandstation::TrainDbConfig);
CDI_GROUP_END();

/// The main structure of the CDI. ConfigDef is the symbol we use in main.cxx
/// to refer to the configuration defined here.
CDI_GROUP(ConfigDef, MainCdi());
/// Adds the <identification> tag with the values from SNIP_STATIC_DATA above.
CDI_GROUP_ENTRY(ident, Identification);
/// Adds an <acdi> tag.
CDI_GROUP_ENTRY(acdi, Acdi);
/// Adds a segment for changing the values in the ACDI user-defined
/// space. UserInfoSegment is defined in the system header.
CDI_GROUP_ENTRY(userinfo, UserInfoSegment);
/// Adds the main configuration segment.
CDI_GROUP_ENTRY(seg, BaseSegment);
/// A segment for the train database.
CDI_GROUP_ENTRY(trains, TrainSegment);
//CDI_GROUP_ENTRY(ro_seg, ROSegment);
CDI_GROUP_END();

} // namespace nmranet




#endif // _BRACZ_CS_TIVA_CONFIG_HXX_
