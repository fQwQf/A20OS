# A20OS cli world — root's interactive shell configuration.
#
# The world ships Alpine's stock zsh; this only adds what a plain command-line
# box is expected to have (history, colours, aliases).  Nothing here is
# A20OS-specific: it is the same set of conveniences a normal Alpine install
# would give an interactive user.

# History is kept in /dev/shm, which the kernel already mounts as tmpfs under
# /extra before the chroot.  $HOME is on the world image's journal-less ext4, and
# that is exactly where history must not go -- see the setopt note below.
HISTFILE=/dev/shm/.zsh_history
HISTSIZE=5000
SAVEHIST=5000
# Deliberately NOT setting SHARE_HISTORY / INC_APPEND_HISTORY.  Both make zsh
# rewrite the history file after *every* command, and on riscv64 each rewrite
# leaves an unreaped orphan inode on the world image's ext4 (measured: one per
# command, zero with them off).  Enough of them and the image refuses to mount
# on the next boot -- "[EXT4] refusing mount: orphan recovery failed (-95)";
# the cause is A20OS' ext4 orphan reap, see packages/world/cli.world.
# Plain zsh history (in-memory, written only at shell exit) produces no churn,
# and it still gives a working `history` within the session.
setopt HIST_IGNORE_DUPS HIST_IGNORE_SPACE
setopt AUTO_CD NO_BEEP INTERACTIVE_COMMENTS

autoload -Uz colors && colors
autoload -Uz compinit && compinit -u 2>/dev/null

# Green user@host, blue cwd; the right prompt only appears when the last
# command failed, so a clean shell has no trailing noise.
PROMPT='%F{green}%n@%m%f %F{blue}%~%f %# '
RPROMPT='%(?..%F{red}[%?]%f)'

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
