// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QString>

// manual and modification docs
// Наш мануал — набор markdown-глав (docs/manual). Локально рядом с exe
// лежит копия каталога, иначе открывается оглавление на GitHub.
const QString UserManualName( "manual/README.md" );
const QString ACModificationName( "HANTEK6022_AC_Modification.pdf" );
const QString FrequencyGeneratorModificationName( "HANTEK6022_Frequency_Generator_Modification.pdf" );

// where are the (local) documents?
#if defined( Q_OS_WIN )
const QString DocPath( "documents\\" );
#elif defined( Q_OS_FREEBSD )
const QString DocPath( "/usr/local/share/doc/ctpu-bintape-timechannel/" );
#else
const QString DocPath( "/usr/share/doc/ctpu-bintape-timechannel/" );
#endif

// GitHub doc location
const QString DocUrl( "https://github.com/dtba3a-del/-Ctpu-bintape-timechannel-/blob/main/docs/" );
