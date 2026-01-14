
ListenManager::~ListenManager() 
{
    if (HasFd()) 
    {
        /* Cache fd before DelFd() to prevent use-after-free */
        
        int fd = GetFd();
        SocketEngine::DelFd(this);
        
        if (fd >= 0) 
        {
            close(fd);
        }
        
        Instance->Logs->Debug("listenmanager", "ListenManager destroyed");
    }
}
