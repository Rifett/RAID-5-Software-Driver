# RAID 5 Software Driver

This project provides a C++ implementation of a RAID (Redundant Array of Independent Disks) system. 
Supports the creation, starting, stopping, resynchronizing, and status checking of RAID volumes. 
The implementation also includes support for disk metadata management, parity calculations, and disk failure handling.

## Features
- **Create**: Initialize a new RAID volume with standard metadata.
- **Start**: Start the RAID volume, check and collect metadata from all disks, and update the system state.
- **Stop**: Stop the RAID volume and update metadata on all disks.
- **Resync**: Resynchronize the RAID volume by restoring data on a failed disk using parity calculations.
- **Read**: Read data from the RAID volume with error handling and data recovery using parity.
- **Write**: Write data to the RAID volume with error handling and data integrity checks.
- **Status**: Check the current status of the RAID volume.
- **Size**: Get the total usable size of the RAID volume.

## Usage
Project can be used as a RAID 5 driver: for data-related operations with error tolerance of one failing data source. 
In case of changing disk emulations (files) to be real disks with all provided operations, implementation can be used as a normal storage driver that operates on multiple disks.
