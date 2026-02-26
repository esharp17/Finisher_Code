const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('finisher', {
  getVersion: () => ipcRenderer.invoke('app:getVersion'),
  serial: {
    listPorts: () => ipcRenderer.invoke('serial:listPorts'),
    autoConnect: () => ipcRenderer.invoke('serial:autoConnect'),
    connect: (options) => ipcRenderer.invoke('serial:connect', options),
    disconnect: () => ipcRenderer.invoke('serial:disconnect'),
    sendLine: (line) => ipcRenderer.invoke('serial:sendLine', line),
    getConnectionInfo: () => ipcRenderer.invoke('serial:getConnectionInfo'),
    getLastStatus: () => ipcRenderer.invoke('serial:getLastStatus'),
    onLine: (handler) => {
      const listener = (_evt, line) => handler(line);
      ipcRenderer.on('serial:line', listener);
      return () => ipcRenderer.removeListener('serial:line', listener);
    },
    onConnectionChange: (handler) => {
      const listener = (_evt, info) => handler(info);
      ipcRenderer.on('serial:connection', listener);
      return () => ipcRenderer.removeListener('serial:connection', listener);
    }
  }
});
