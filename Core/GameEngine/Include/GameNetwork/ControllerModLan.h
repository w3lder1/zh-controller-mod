/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// ControllerModLan.h /////////////////////////////////////////////////////////////////////////////
// ControllerMod @feature Controller Mod players only play LAN games with each other.
//
// Two copies of the game stay in sync only when they run the same program on the same gameplay
// data. Zero Hour never checked that on LAN (the join check has been off since the original game),
// so a Controller Mod player could join normal Zero Hour, or another Controller Mod version, and
// the match would go out of sync later. Now:
// - A Controller Mod host refuses joiners whose program or gameplay data differ. Their join request
//   already carries both checksums (exeCRC, iniCRC).
// - A Controller Mod host tags what it sends: its game announcements (in the options text buffer,
//   at its end, when the options leave room there) and every join acceptance (in the packet space
//   an acceptance doesn't use). Every other version of the game ignores these bytes, so nothing
//   changes for normal Zero Hour players. A tag holds a marker, the mod version and one fingerprint
//   of program + gameplay data.
// - A Controller Mod player doesn't ask to join a listed game whose announcement shows it can't be
//   joined, and leaves at once when a host accepts it without a matching tag (normal Zero Hour, an
//   older or another Controller Mod). The LAN list shows those games greyed out, with what they are.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "Common/ControllerModVersion.h"
#include "Common/GlobalData.h"

class LANGameInfo;

/// The LAN join check (LANAPI::handleRequestJoin) is on in Zero Hour builds with the Controller Mod.
#define CONTROLLERMOD_LAN_VERSION_CHECK 1

namespace ControllerModLan
{
	enum
	{
		TAG_BYTES = 8   ///< marker (2), version (2), fingerprint (4)
	};

	/// What a game announcement says (LANGameInfo::getControllerModTagState).
	enum TagState
	{
		TAG_UNKNOWN = 0,   ///< not heard yet, or the options filled the buffer (no room for a tag)
		TAG_ABSENT,        ///< room for a tag but none: normal Zero Hour or an older Controller Mod
		TAG_PRESENT
	};

	/// The mod version packed in 16 bits: major (5 bits), minor (5), patch (6).
	inline UnsignedShort packedVersion()
	{
		const char *text = ZH_CONTROLLER_MOD_VERSION;
#if CONTROLLERMOD_ENABLE_TEST_INPUT
		// Automated tests only: CONTROLLERMOD_TEST_MOD_VERSION pretends to be another version.
		if (const char *test = getenv("CONTROLLERMOD_TEST_MOD_VERSION"))
			text = test;
#endif
		Int major = 0, minor = 0, patch = 0;
		sscanf(text, "%d.%d.%d", &major, &minor, &patch);
		return (UnsignedShort)(((major & 31) << 11) | ((minor & 31) << 6) | (patch & 63));
	}

	inline UnicodeString versionText(UnsignedShort packed)
	{
		UnicodeString text;
		text.format(L"%d.%d.%d", (packed >> 11) & 31, (packed >> 6) & 31, packed & 63);
		return text;
	}

	/// One number for "same program and same gameplay data".
	inline UnsignedInt fingerprint()
	{
		UnsignedInt h = TheGlobalData->m_exeCRC;
		h = (h ^ (TheGlobalData->m_iniCRC + 0x9E3779B9u + (h << 6) + (h >> 2)));
		return h;
	}

	inline void writeTag(UnsignedByte *at)
	{
		const UnsignedShort marker = 0x5A43;
		const UnsignedShort version = packedVersion();
		const UnsignedInt print = fingerprint();
		memcpy(at, &marker, 2);
		memcpy(at + 2, &version, 2);
		memcpy(at + 4, &print, 4);
	}

	inline Bool readTag(const UnsignedByte *at, UnsignedShort *version, UnsignedInt *print)
	{
		UnsignedShort marker = 0;
		memcpy(&marker, at, 2);
		if (marker != 0x5A43)
			return FALSE;
		memcpy(version, at + 2, 2);
		memcpy(print, at + 4, 4);
		return TRUE;
	}

	/// Announcements: the tag takes the last bytes before the options buffer's final zero, when the
	/// options text ends before them.
	inline void writeOptionsTag(char *options, Int size)
	{
		const Int at = size - 1 - TAG_BYTES;
		if ((Int)strlen(options) < at)
			writeTag((UnsignedByte *)options + at);
	}

	inline TagState readOptionsTag(const char *options, Int size, UnsignedShort *version, UnsignedInt *print)
	{
		const Int at = size - 1 - TAG_BYTES;
		Int length = 0;
		while (length < size && options[length] != 0)
			++length;
		if (length >= at)
			return TAG_UNKNOWN;
		return readTag((const UnsignedByte *)options + at, version, print) ? TAG_PRESENT : TAG_ABSENT;
	}

	/// Automated tests only: CONTROLLERMOD_TEST_ACT_RETAIL makes this copy behave like normal Zero
	/// Hour on LAN (sends no tags, checks nothing); CONTROLLERMOD_TEST_JOIN_CHECK=accept skips the
	/// check of the LAN list before joining, so only the host's acceptance is checked.
	inline Bool testActRetail()
	{
#if CONTROLLERMOD_ENABLE_TEST_INPUT
		return getenv("CONTROLLERMOD_TEST_ACT_RETAIL") != nullptr;
#else
		return FALSE;
#endif
	}

	inline Bool testSkipListCheck()
	{
#if CONTROLLERMOD_ENABLE_TEST_INPUT
		const char *check = getenv("CONTROLLERMOD_TEST_JOIN_CHECK");
		return testActRetail() || (check && strcmp(check, "accept") == 0);
#else
		return FALSE;
#endif
	}

	/// Shows why this game can't be joined (LANAPICallbacks.cpp). tagState, version and print are
	/// what the host sent.
	void showJoinRefusal(Int tagState, UnsignedShort version, UnsignedInt print);
}
