#pragma once

class UMetaHumanPerformance;

namespace UE::MetaHumanVideoImporter
{
// Repairs the out-of-range end frame written by importer 1.0. Only touches
// performances referencing this importer's project-local footage directory.
bool RepairImportedPerformanceRange(UMetaHumanPerformance* Performance);
}
