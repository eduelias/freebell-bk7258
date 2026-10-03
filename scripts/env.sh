# Sourced by the scripts in this directory: loads ~/.config/doorbell-bk7258/env (dotenv:
# KEY=value, # comments, optional quotes) and sets each key ONLY when it is not already set in
# the environment, so `FW_DIR=/x scripts/build.sh` still wins over the file. The scripts apply
# their built-in defaults afterwards with ${VAR:-default}. Python tools use tools/dbenv.py.
# Template with every key: scripts/env.example.
_dbenv_file="$HOME/.config/doorbell-bk7258/env"
if [ -r "$_dbenv_file" ]; then
	while IFS= read -r _dbenv_line || [ -n "$_dbenv_line" ]; do
		_dbenv_s="${_dbenv_line#"${_dbenv_line%%[![:space:]]*}"}"   # ltrim
		case "$_dbenv_s" in ''|'#'*) continue ;; esac               # blank / comment
		_dbenv_s="${_dbenv_s#export }"
		case "$_dbenv_s" in *=*) ;; *) continue ;; esac              # needs KEY=
		_dbenv_key="${_dbenv_s%%=*}"
		_dbenv_key="${_dbenv_key//[[:space:]]/}"
		case "$_dbenv_key" in ''|[0-9]*|*[!A-Za-z0-9_]*) continue ;; esac
		if [ -z "${!_dbenv_key+x}" ]; then
			# eval gives bash quoting/escaping/$HOME semantics, the same as `source` would
			eval "$_dbenv_key=${_dbenv_s#*=}" && export "$_dbenv_key"
		fi
	done < "$_dbenv_file"
fi
unset _dbenv_file _dbenv_line _dbenv_s _dbenv_key
