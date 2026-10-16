#!/bin/bash
# Assemble the giulia (OnePlus 13R / Ace 5, SM8650) SSC HexagonFS root tree.
# Adapted from Osilvfe/out_tree_module sensors/ssc/build-bundle.sh (caihong)
# to use the already-extracted vendor/odm trees under android_extract/.
set -euo pipefail

SRC=${SRC:-/home/DianKuang/Mainline-Kernel/ssc/hexagonrpc}
FW=${FW:-/home/DianKuang/android_extract}
OUT=${OUT:-/home/DianKuang/Mainline-Kernel/ssc/bundle-giulia}
PLATFORM=${PLATFORM:-MTP}
SOC_ID=${SOC_ID:-577}

vendor_sensors=$FW/vendor_ext/etc/sensors
vendor_adsp=$FW/vendor_ext/lib/rfsa/adsp
odm_sensors=$FW/odm_ext/etc/sensor

for p in "$SRC/build/hexagonrpcd/hexagonrpcd" \
         "$SRC/build/tools/sscregistrygen" \
         "$SRC/build/libhexagonrpc/libhexagonrpc.so.0.5" \
         "$vendor_sensors/sns_reg_config" "$vendor_sensors/config/json.lst" \
         "$odm_sensors/config/json_list"; do
	[ -e "$p" ] || { echo "missing: $p" >&2; exit 1; }
done

rm -rf "$OUT"
root=$OUT/root
mkdir -p "$OUT/bin" "$OUT/lib" \
	"$root/sensors/config" "$root/sensors/odm-config" \
	"$root/sensors/registry" "$root/sensors/persist/registry" \
	"$root/dsp/adsp" "$root/socinfo"
vendor_input=$OUT/work/vendor-input
odm_input=$OUT/work/odm-input
mkdir -p "$vendor_input" "$odm_input"

copy_list() { # list srcdir staging
	local list=$1 src=$2 stage=$3
	while IFS= read -r file; do
		file=${file%$'\r'}
		[ -n "$file" ] || continue
		[ -f "$src/$file" ] || { echo "  (skip missing $file)"; continue; }
		cp "$src/$file" "$stage/$file"
	done < "$list"
}

# vendor /etc/sensors/config per json.lst
copy_list "$vendor_sensors/config/json.lst" "$vendor_sensors/config" "$vendor_input"
# odm /etc/sensor/config per json_list
copy_list "$odm_sensors/config/json_list" "$odm_sensors/config" "$odm_input"

# merged config dir served inside HexagonFS
cp -a "$vendor_input/." "$root/sensors/config/"
cp -a "$odm_input/." "$root/sensors/config/"
cp -a "$odm_sensors/config/." "$root/sensors/odm-config/"
cp -a "$vendor_adsp/." "$root/dsp/adsp/"

# combined json.lst (vendor first, then odm)
{ sed 's/\r$//' "$vendor_sensors/config/json.lst"
  sed 's/\r$//' "$odm_sensors/config/json_list"; } > "$root/sensors/config/json.lst"

# rewrite config path to the HexagonFS vendor mount
sed 's#file=config=/odm/etc/sensor/config#file=config=/vendor/etc/sensors/config#' \
	"$vendor_sensors/sns_reg_config" > "$root/sensors/sns_reg.conf"

# generate the registry: vendor first, then odm overrides
"$SRC/build/tools/sscregistrygen" -p "$PLATFORM" -s "$SOC_ID" "$vendor_input" "$root/sensors/registry"
"$SRC/build/tools/sscregistrygen" -p "$PLATFORM" -s "$SOC_ID" "$odm_input" "$root/sensors/registry"
cp -a "$root/sensors/registry/." "$root/sensors/persist/registry/"

printf '%s\n' "$PLATFORM" > "$root/socinfo/hw_platform"
printf '0\n'  > "$root/socinfo/platform_subtype"
printf '0\n'  > "$root/socinfo/platform_subtype_id"
printf '65536\n' > "$root/socinfo/platform_version"
printf '%s\n' "$SOC_ID" > "$root/socinfo/soc_id"
printf '2.0\n' > "$root/socinfo/revision"

install -m 0755 "$SRC/build/hexagonrpcd/hexagonrpcd" "$OUT/bin/hexagonrpcd"
install -m 0755 "$SRC/build/libhexagonrpc/libhexagonrpc.so.0.5" "$OUT/lib/libhexagonrpc.so.0.5"

rm -rf "$OUT/work"

echo "registry files: $(find "$root/sensors/registry" -maxdepth 1 -type f | wc -l)"
echo "bundle: $OUT"
