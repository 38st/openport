const copyButton = document.querySelector('.copy-command');
const command = document.querySelector('#install-command');
const copyStatus = document.querySelector('#copy-status');
let copyReset;
let copying = false;

copyButton.hidden = false;
copyButton.addEventListener('click', async () => {
  if (copying) return;
  copying = true;
  clearTimeout(copyReset);
  copyStatus.textContent = '';
  try {
    await navigator.clipboard.writeText(command.querySelector('code').textContent.trim());
    copyButton.textContent = 'Copied!';
    copyStatus.textContent = 'Command copied to clipboard.';
  } catch {
    // Keep manual copying possible when clipboard access is unavailable or denied.
    command.focus();
    const range = document.createRange();
    range.selectNodeContents(command);
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    copyStatus.textContent = 'Couldn’t copy automatically. The command is selected; use your device’s Copy action.';
  } finally {
    copying = false;
    copyReset = setTimeout(() => { copyButton.textContent = 'Copy command'; }, 2500);
  }
});

const videos = [...document.querySelectorAll('.demo video')];
const mobileLayout = window.matchMedia('(max-width: 700px)');
// A change in layout must never leave the hidden recording playing.
mobileLayout.addEventListener('change', () => videos.forEach(video => video.pause()));
document.addEventListener('visibilitychange', () => {
  if (document.hidden) videos.forEach(video => video.pause());
});
