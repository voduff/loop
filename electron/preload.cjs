'use strict';
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('loop', {
  initial: () => ipcRenderer.invoke('loop:initial'),
  action: (type, value) => ipcRenderer.invoke('loop:action', type, value),
  subscribe: callback => {
    const listener = (_event, message) => callback(message);
    ipcRenderer.on('loop:event', listener);
    return () => ipcRenderer.removeListener('loop:event', listener);
  }
});
