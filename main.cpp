#ifndef __PROGTEST__
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <stdexcept>
using namespace std;

constexpr int                          SECTOR_SIZE                             =             512;
constexpr int                          MAX_RAID_DEVICES                        =              16;
constexpr int                          MAX_DEVICE_SECTORS                      = 1024 * 1024 * 2;
constexpr int                          MIN_DEVICE_SECTORS                      =    1 * 1024 * 2;

constexpr int                          RAID_STOPPED                            = 0;
constexpr int                          RAID_OK                                 = 1;
constexpr int                          RAID_DEGRADED                           = 2;
constexpr int                          RAID_FAILED                             = 3;

struct TBlkDev
{
    int                                  m_Devices;
    int                                  m_Sectors;
    int                               (* m_Read )  ( int, int, void *, int );
    int                               (* m_Write ) ( int, int, const void *, int );
};
#endif /* __PROGTEST__ */

struct Metadata
{
    void resetDisksStatus() {
        for (int diskIndex = 0; diskIndex < MAX_RAID_DEVICES; ++diskIndex)
            disksStatus[diskIndex] = false;
    }

    bool operator==(const Metadata &other) const {
        for (int i = 0; i < MAX_RAID_DEVICES; ++i) {
            if ( other.disksStatus[i] != this->disksStatus[i] )
                return false;
        }
        if ( other.raidStatus != this->raidStatus )
            return false;
        return true;
    };

    bool disksStatus[MAX_RAID_DEVICES] = { false };
    int raidStatus = RAID_STOPPED;
};

class CRaidVolume
{
public:
    CRaidVolume() = default;


    static bool create ( const TBlkDev& dev )
    {
        //Initialize standard metadata
        Metadata data;

        //Write this metadata to each disk at the last sector
        for (int diskIndex = 0; diskIndex < dev.m_Devices; ++diskIndex)
            if ( dev.m_Write(diskIndex, dev.m_Sectors - 1, &data, 1) != 1)
                return false;
        return true;
    }


    int start ( const TBlkDev& dev )
    {
        //Check current status
        if ( systemState.raidStatus != RAID_STOPPED ) return systemState.raidStatus;

        //Some variables
        Metadata disksMetadata[MAX_RAID_DEVICES];
        std::byte buffer[SECTOR_SIZE]{};

        //Copy the device
        this->device = dev;

        //Mark all disks as working ones
        systemState.resetDisksStatus();

        //Collect metadata from disks and mark unavailable ones
        for (int diskIndex = 0; diskIndex < dev.m_Devices; ++diskIndex) {
            if ( dev.m_Read(diskIndex, dev.m_Sectors - 1, buffer, 1) != 1 ) {
                systemState.disksStatus[diskIndex] = true;
                continue;
            }
            memcpy(&disksMetadata[diskIndex].disksStatus, buffer, MAX_RAID_DEVICES);
            memcpy(&disksMetadata[diskIndex].raidStatus, &buffer[MAX_RAID_DEVICES], sizeof(int));
        }
        
        //Mark all disks with wrong metadata
        Metadata standardMetadata = getStandardMetadata(disksMetadata);
        markWrongMetadataDisks(disksMetadata, standardMetadata);
        
        //Check the RAID system status 
        if ( countFailedDisks() > 1 ) return systemState.raidStatus = RAID_FAILED;
        else if ( countFailedDisks() == 1) return systemState.raidStatus = RAID_DEGRADED;
        return systemState.raidStatus = RAID_OK;
    }


    int stop ()
    {
        //Insert current metadata into all the disks
        for (int diskIndex = 0; diskIndex < this->device.m_Devices; ++diskIndex)
            this->device.m_Write(diskIndex, this->device.m_Sectors - 1, &systemState, 1);

        //Update the status
        return systemState.raidStatus = RAID_STOPPED;
    }


    int resync ()
    {
        //Check the status
        if ( systemState.raidStatus != RAID_DEGRADED )
            return systemState.raidStatus;

        //Some variables
        int failedDiskIndex = getFailedDiscIndex();
        std::byte buffer[SECTOR_SIZE]{};

        //Go through all sectors (except for the bottom one with metadata) of previously failed disk and restore them
        for (int sectorIndex = 0; sectorIndex < device.m_Sectors - 1; ++sectorIndex) {
            //Calculate parity of the sector from the stripe and put it in the buffer; in case another disk failed, memory is lost
            if ( !calculateParity(failedDiskIndex, sectorIndex, buffer) )
                return systemState.raidStatus = RAID_FAILED;

            //If attempt to write failed, disk is still not working, same state
            if ( device.m_Write(failedDiskIndex, sectorIndex, buffer, 1) != 1 )
                return systemState.raidStatus;
        }

        systemState.resetDisksStatus();
        return systemState.raidStatus = RAID_OK;
    }


    int status () const
    {
        return systemState.raidStatus;
    }


    int size () const
    {
        return (device.m_Devices - 1) * (device.m_Sectors - 1);
    }


    bool read ( int secNr, void * data, int secCnt )
    {
        //Status check
        if ( systemState.raidStatus == RAID_STOPPED || systemState.raidStatus == RAID_FAILED )
            return false;

        //Some variables
        std::byte buffer[SECTOR_SIZE]{};

        //Main loop that goes through all the sectors that we need to read
        for (int absoluteSectorIndex = secNr; absoluteSectorIndex < secNr + secCnt; ++absoluteSectorIndex) {
            auto [diskIndex, diskSector] = getRelativeIndexes(absoluteSectorIndex);

            //Read with the system state checking
            if ( !checkedRead(diskIndex, diskSector, buffer) )
                return false;

            //Transfer read data into the reading destination
            memcpy( ( std::byte* ) data + ( absoluteSectorIndex - secNr ) * SECTOR_SIZE, buffer, SECTOR_SIZE);
        }
        return true;
    }


    bool write ( int secNr, const void * data, int secCnt )
    {
        //Status check
        if ( systemState.raidStatus == RAID_STOPPED || systemState.raidStatus == RAID_FAILED )
            return false;

        //Some variables
        std::byte buffer[SECTOR_SIZE]{};

        //Main loop that goes through all the sectors that we need to write into
        for (int absoluteSectorIndex = secNr; absoluteSectorIndex < secNr + secCnt; ++absoluteSectorIndex) {
            //Transfer input data sector into the buffer
            memcpy(buffer,  ( std::byte* ) data + ( absoluteSectorIndex - secNr ) * SECTOR_SIZE, SECTOR_SIZE);

            //Some more variables
            auto [diskIndex, diskSector] = getRelativeIndexes(absoluteSectorIndex);

            //Write the data with system state checks
            if ( !checkedWrite(diskIndex, diskSector, buffer) )
                return false;
        }
        return true;
    }


protected:
    Metadata getStandardMetadata (Metadata* disksMetadata) const
    {
        if ( device.m_Devices <= 0) return {};
        if ( device.m_Devices < 3 ) return disksMetadata[0];
        return disksMetadata[0] == disksMetadata[1] ? disksMetadata[0] : disksMetadata[2];
    }

    void markWrongMetadataDisks(Metadata* disksMetadata, Metadata& standardMetadata)
    {
        for (int diskIndex = 0; diskIndex < device.m_Devices; ++diskIndex) {
            if ( disksMetadata[diskIndex] != standardMetadata )
                systemState.disksStatus[diskIndex] = true;
        }
    }
    
    int countFailedDisks()
    {
        int counter = 0;
        for (int diskIndex = 0; diskIndex < device.m_Devices; ++diskIndex) {
            if ( systemState.disksStatus[diskIndex] )
                ++counter;
        }
        return counter;
    }

    int getFailedDiscIndex()
    {
        for (int diskIndex = 0; diskIndex < device.m_Devices; ++diskIndex) {
            if ( systemState.disksStatus[diskIndex] )
                return diskIndex;
        }
        return MAX_RAID_DEVICES;
    }

    bool calculateParity(int failedDiskIndex, int sectorIndex, std::byte *parityBuffer)
    {
        std::byte buffer[SECTOR_SIZE]{};
        memset(parityBuffer, 0, SECTOR_SIZE);
        for (int diskIndex = 0; diskIndex < device.m_Devices; ++diskIndex) {
            if ( diskIndex == failedDiskIndex ) continue;
            if ( device.m_Read(diskIndex, sectorIndex, buffer, 1) != 1 ) {
                systemState.disksStatus[diskIndex] = true;
                systemState.raidStatus = RAID_FAILED;
                return false;
            }
            xorBuffer(parityBuffer, buffer);
        }
        return true;

    }

    static void xorBuffer(std::byte *lhsBuffer, std::byte *rhsBuffer)
    {
        for (int i = 0; i < SECTOR_SIZE; ++i) {
            lhsBuffer[i] ^= rhsBuffer[i];
        }

    }

    std::pair<int, int> getRelativeIndexes(int sectorIndex) const
    {
        int shift = device.m_Devices * (device.m_Devices - 1);
        sectorIndex += (int) (sectorIndex / device.m_Devices) + 1 + (int)(sectorIndex / shift);
        return make_pair(sectorIndex % device.m_Devices, (int) (sectorIndex / device.m_Devices));
    }

    bool checkedRead(int diskIndex, int sectorIndex, std::byte *destination)
    {
        //Reading from a normal disk
        if ( !systemState.disksStatus[diskIndex] ) {
            if ( device.m_Read(diskIndex, sectorIndex, destination, 1) != 1 ) {
                //Failed to read in normal state -> update current metadata and try to read the same sector as failed one
                if (systemState.raidStatus == RAID_OK) {
                    systemState.raidStatus = RAID_DEGRADED;
                    systemState.disksStatus[diskIndex] = true;
                }
                //Failed in degraded state -> Disk failed
                else {
                    systemState.raidStatus = RAID_FAILED;
                    systemState.disksStatus[diskIndex] = true;
                    return false;
                }
            }
        }
        //Reading from a failed disk -> parity needs to be calculated
        if ( systemState.disksStatus[diskIndex] ) {
            if ( !calculateParity(diskIndex, sectorIndex, destination) ) {
                systemState.raidStatus = RAID_FAILED;
                return false;
            }
        }
        return true;
    }

    bool checkedWrite(int diskIndex, int sectorIndex, std::byte *source) {
        std::byte parityBuffer[SECTOR_SIZE]{}, oldSectorData[SECTOR_SIZE]{};
        int parityDiskIndex = sectorIndex % device.m_Devices;

        //Read the old sector data
        if ( !checkedRead(diskIndex, sectorIndex, oldSectorData) )
            return false;

        //Read the old parity
        if (!checkedRead(parityDiskIndex, sectorIndex, parityBuffer))
            return false;

        //If disk still works, write the new sector data
        if ( !systemState.disksStatus[diskIndex] && !atomicWrite(diskIndex, sectorIndex, source))
            return false;

        //If parity disk still works, calculate new parity and update it
        if ( !systemState.disksStatus[parityDiskIndex] ) {
            //First xor to remove old data, second xor to include the new one
            xorBuffer(parityBuffer, oldSectorData);
            xorBuffer(parityBuffer, source);

            //Write the new parity
            if ( !atomicWrite(parityDiskIndex, sectorIndex, parityBuffer) )
                return false;
        }

        return true;
    }

    bool atomicWrite( int diskIndex, int sectorIndex, std::byte *source ) {
        if ( device.m_Write(diskIndex, sectorIndex, source, 1) != 1 ) {
            //Failed to write in normal state -> update current metadata, that's fine
            if (systemState.raidStatus == RAID_OK) {
                systemState.raidStatus = RAID_DEGRADED;
                systemState.disksStatus[diskIndex] = true;
            }
            //Failed in degraded state -> Disk failed
            else {
                systemState.raidStatus = RAID_FAILED;
                systemState.disksStatus[diskIndex] = true;
                return false;
            }
        }
        return true;
    }

    TBlkDev device = TBlkDev();
    Metadata systemState;
};

#ifndef __PROGTEST__
#include "tests.cpp"

#endif /* __PROGTEST__ */
