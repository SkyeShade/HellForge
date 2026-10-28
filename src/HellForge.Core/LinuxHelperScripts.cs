namespace HellForge.Core;

// Bash shared by the Linux helpers the launcher starts after it exits. Both helpers set $prefix first and then include
// PrefixInUseFunction at PrefixInUseMarker: it finds this user's processes that have WINEPREFIX=<prefix>/pfx or an
// executable or working folder under the prefix, and leaves their names in $pfx_in_use_names for the failure line.
public static class LinuxHelperScripts
{
    public const string PrefixInUseMarker = "@PREFIX_IN_USE@";

    public const string PrefixInUseFunction = """
        pfx_in_use_names=""
        add_in_use() {
          local name
          name=$(cat "/proc/$1/comm" 2>/dev/null || true)
          [ -n "$name" ] || name="pid $1"
          pfx_in_use_names="${pfx_in_use_names}${pfx_in_use_names:+, }$name"
        }
        prefix_in_use() {
          local dir pid exe cwd
          pfx_in_use_names=""
          # readlink prints absolute paths and WINEPREFIX is compared as written; nothing here is resolved against the
          # helper's working directory. A relative prefix cannot name one of those values, so it matches nothing.
          case "$prefix" in /*) ;; *) return 1 ;; esac
          for dir in /proc/[0-9]*; do
            pid=${dir##*/}
            [ "$pid" = "$$" ] && continue
            [ -O "$dir" ] || continue
            if [ -r "$dir/environ" ] &&
               { tr '\0' '\n' <"$dir/environ"; } 2>/dev/null | grep -Fxq -- "WINEPREFIX=$prefix/pfx"; then
              add_in_use "$pid"; continue
            fi
            exe=$(readlink "$dir/exe" 2>/dev/null || true)
            cwd=$(readlink "$dir/cwd" 2>/dev/null || true)
            case "$exe" in "$prefix"|"$prefix"/*) add_in_use "$pid"; continue ;; esac
            case "$cwd" in "$prefix"|"$prefix"/*) add_in_use "$pid" ;; esac
          done
          [ -n "$pfx_in_use_names" ]
        }
        """;

    public static string IncludePrefixInUse(string template) =>
        template.Replace(PrefixInUseMarker, PrefixInUseFunction, StringComparison.Ordinal);
}
