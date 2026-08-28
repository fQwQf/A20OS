/* A20OS landing — copy-to-clipboard for the quick-start commands.
   Vanilla, no dependencies, no animation logic. */
(function () {
  'use strict';

  var copyBtn = document.querySelector('[data-copy]');
  if (!copyBtn) return;

  var COMMANDS = [
    'git clone https://github.com/fQwQf/A20OS.git',
    'cd A20OS',
    'make ARCH=riscv64 BOARD=qemu-virt-riscv64 run'
  ].join('\n');

  copyBtn.addEventListener('click', function () {
    var done = function () {
      copyBtn.textContent = '已复制';
      window.setTimeout(function () {
        copyBtn.textContent = '复制';
      }, 1500);
    };

    if (navigator.clipboard && navigator.clipboard.writeText) {
      navigator.clipboard.writeText(COMMANDS).then(done, done);
    } else {
      var ta = document.createElement('textarea');
      ta.value = COMMANDS;
      ta.style.position = 'fixed';
      ta.style.opacity = '0';
      document.body.appendChild(ta);
      ta.select();
      try { document.execCommand('copy'); } catch (e) { /* no-op */ }
      document.body.removeChild(ta);
      done();
    }
  });
})();
