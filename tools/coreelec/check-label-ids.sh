#!/bin/sh
# CoreELEC rebase check: upstream must not allocate info label ids inside the
# CE ranges (ledger in xbmc/guilib/guiinfo/CEGUIInfoRegistry.h). Run against
# the tree after every upstream merge; compile time asserts only catch
# renumbering, this catches upstream growth into the CE ranges.
set -eu

H="$(git rev-parse --show-toplevel)/xbmc/guilib/guiinfo/GUIInfoLabels.h"
status=0

if grep -nE 'PLAYER_PROCESS_START \+ [3-9][0-9]' "$H"; then
  echo "ERROR: upstream allocated Player.Process ids in the CE range (+30..+99)" >&2
  status=1
fi

if grep -nE '= 15[3-9][0-9];' "$H"; then
  echo "ERROR: upstream allocated literal ids in the CE range 1530..1599" >&2
  status=1
fi

if grep -nE '= 8[0-9]{3};' "$H"; then
  echo "ERROR: upstream allocated ids in the CE range 8000..8999" >&2
  status=1
fi

[ "$status" -eq 0 ] && echo "CE label id ranges clean"
exit $status
