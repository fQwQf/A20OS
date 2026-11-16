# A20OS cli world — root's interactive shell configuration.
#
# The world ships Alpine's stock zsh; this only adds what a plain command-line
# box is expected to have (history, colours, aliases).  Nothing here is
# A20OS-specific: it is the same set of conveniences a normal Alpine install
# would give an interactive user.

# Keep history in tmpfs and write it at shell exit. The ordinary ext4 orphan
# lifecycle has been repaired; this remains the workstation's history policy.
HISTFILE=/dev/shm/.zsh_history
HISTSIZE=5000
SAVEHIST=5000
# SHARE_HISTORY / INC_APPEND_HISTORY remain optional shell preferences.
setopt HIST_IGNORE_DUPS HIST_IGNORE_SPACE
setopt AUTO_CD NO_BEEP INTERACTIVE_COMMENTS

autoload -Uz colors && colors
autoload -Uz compinit && compinit -u 2>/dev/null

# Green user@host, blue cwd.  No RPROMPT: a right prompt forces ZLE to position
# the cursor absolutely and erase the right-hand side on every redraw, which is
# the first thing to look wrong when the console width is off.  $? is still
# there if you want it, and `resize-console` fixes the width itself.
#
# The width comes from $COLUMNS, which the stage-2 init sets by asking the
# terminal (see /usr/lib/a20/console-size).  Without it zsh would wrap against
# the console tty's hardcoded 80 columns and garble anything longer.
PROMPT='%F{green}%n@%m%f %F{blue}%~%f %# '

# An external command cannot export dimensions back into its parent shell.
# Keep the interactive update here; the helper owns query/error handling.
resize-console() {
    local resize_assign
    resize_assign=$(/usr/local/bin/resize-console --shell) || return
    eval "$resize_assign"
    export COLUMNS LINES
    print -r -- "console is now ${COLUMNS}x${LINES}"
}

# GNU tools live in /usr/bin, busybox applets in /bin; this ordering is what
# makes the GNU versions win (see packages/world/cli.world).
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PAGER=less LESS='-R' EDITOR=vim

alias ls='ls --color=auto'
alias ll='ls -alh --color=auto'
alias la='ls -A --color=auto'
alias l='ls -CF --color=auto'
alias grep='grep --color=auto'
alias diff='diff --color=auto'
alias ip='ip -color=auto'
alias ..='cd ..'
alias ...='cd ../..'
