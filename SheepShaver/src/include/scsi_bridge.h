/*
 *  scsi_bridge.h - SCSI bridge driver for protocol discovery
 *
 *  SheepShaver (C) 1997-2008 Christian Bauer and Marc Hellwig
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef SCSI_BRIDGE_H
#define SCSI_BRIDGE_H

const int SCSIBridgeRefNum = -49;			// RefNum — replaces .EDisk in unit table
const uint16 SCSIBridgeDriverFlags = 0x6f00;	// Driver flags

extern int16 SCSIBridgeOpen(uint32 pb, uint32 dce);
extern int16 SCSIBridgePrime(uint32 pb, uint32 dce);
extern int16 SCSIBridgeControl(uint32 pb, uint32 dce);
extern int16 SCSIBridgeStatus(uint32 pb, uint32 dce);

#endif
