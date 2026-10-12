use crate::runtime::Runtime;
use std::{
    io,
    sync::{Arc, atomic::Ordering},
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::windows::named_pipe::{NamedPipeServer, ServerOptions},
    sync::{Semaphore, watch},
    task::JoinSet,
    time::{Duration, timeout},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, LocalFree},
    Security::{
        Authorization::{
            ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        },
        GetTokenInformation, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER, TokenUser,
    },
    System::Threading::{GetCurrentProcess, OpenProcessToken},
};

struct Security {
    descriptor: *mut std::ffi::c_void,
}
impl Drop for Security {
    fn drop(&mut self) {
        unsafe {
            LocalFree(self.descriptor);
        }
    }
}
impl Security {
    fn new() -> io::Result<Self> {
        unsafe {
            let mut token: HANDLE = std::ptr::null_mut();
            if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
                return Err(io::Error::last_os_error());
            }
            let mut size = 0;
            GetTokenInformation(token, TokenUser, std::ptr::null_mut(), 0, &mut size);
            let mut buffer = vec![0usize; (size as usize).div_ceil(std::mem::size_of::<usize>())];
            let result = GetTokenInformation(
                token,
                TokenUser,
                buffer.as_mut_ptr().cast(),
                size,
                &mut size,
            );
            let error = io::Error::last_os_error();
            CloseHandle(token);
            if result == 0 {
                return Err(error);
            }
            let user = &*buffer.as_ptr().cast::<TOKEN_USER>();
            let mut sid = std::ptr::null_mut();
            if ConvertSidToStringSidW(user.User.Sid, &mut sid) == 0 {
                return Err(io::Error::last_os_error());
            }
            let mut length = 0;
            while *sid.add(length) != 0 {
                length += 1;
            }
            let text = String::from_utf16_lossy(std::slice::from_raw_parts(sid, length));
            LocalFree(sid.cast());
            let sddl: Vec<u16> = format!("D:(A;;FRFW;;;{text})S:(ML;;NW;;;ME)\0")
                .encode_utf16()
                .collect();
            let mut descriptor = std::ptr::null_mut();
            if ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.as_ptr(),
                1,
                &mut descriptor,
                std::ptr::null_mut(),
            ) == 0
            {
                return Err(io::Error::last_os_error());
            }
            Ok(Self { descriptor })
        }
    }
    fn create(&self, name: &str, first: bool) -> io::Result<NamedPipeServer> {
        let mut sa = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: self.descriptor,
            bInheritHandle: 0,
        };
        unsafe {
            ServerOptions::new()
                .first_pipe_instance(first)
                .in_buffer_size(65536)
                .out_buffer_size(65536)
                .create_with_security_attributes_raw(
                    name,
                    (&mut sa as *mut SECURITY_ATTRIBUTES).cast(),
                )
        }
    }
}
pub async fn serve(runtime: Arc<Runtime>, name: &str) -> io::Result<()> {
    let security = Security::new()?;
    let mut listeners = JoinSet::new();
    for index in 0..16 {
        let server = security.create(name, index == 0)?;
        listeners.spawn(listen(server));
    }
    let limit = Arc::new(Semaphore::new(64));
    let (stop_sender, stop) = watch::channel(false);
    let mut clients = JoinSet::new();
    let signal = tokio::signal::ctrl_c();
    tokio::pin!(signal);
    let mut tick = tokio::time::interval(Duration::from_millis(25));
    let result = loop {
        tokio::select! {
            connected=listeners.join_next()=>{
                let connected=match connected {Some(Ok(Ok(pipe)))=>pipe,Some(Ok(Err(e)))=>break Err(e),_=>break Err(io::Error::other("pipe listener stopped"))};
                let next=match security.create(name,false){Ok(next)=>next,Err(error)=>break Err(error)};
                listeners.spawn(listen(next));
                if let Ok(permit)=limit.clone().try_acquire_owned(){let runtime=runtime.clone();let stop=stop.clone();clients.spawn(async move{let _permit=permit;let _=client(connected,runtime,stop).await;});}
            }
            _=tick.tick()=>{if runtime.shutdown.load(Ordering::Acquire){break Ok(());}}
            _=&mut signal=>{break Ok(());}
            Some(_)=clients.join_next(),if !clients.is_empty()=>{}
        }
    };
    listeners.abort_all();
    while listeners.join_next().await.is_some() {}
    let _ = stop_sender.send(true);
    while clients.join_next().await.is_some() {}
    result
}
async fn listen(server: NamedPipeServer) -> io::Result<NamedPipeServer> {
    server.connect().await?;
    Ok(server)
}
async fn client(
    mut pipe: NamedPipeServer,
    runtime: Arc<Runtime>,
    mut stop: watch::Receiver<bool>,
) -> io::Result<()> {
    let mut buffer = Vec::with_capacity(4096);
    let mut chunk = [0; 4096];
    loop {
        let count = tokio::select! {_ = stop.changed()=>return Ok(()),result=timeout(Duration::from_secs(5),pipe.read(&mut chunk))=>result.map_err(|_|io::Error::new(io::ErrorKind::TimedOut,"idle pipe"))??};
        if count == 0 {
            return Ok(());
        }
        buffer.extend_from_slice(&chunk[..count]);
        while let Some(nl) = buffer.iter().position(|b| *b == b'\n') {
            if nl > 8 * 1024 * 1024 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "request exceeds 8 MB",
                ));
            }
            let line = String::from_utf8_lossy(&buffer[..nl])
                .trim_end_matches('\r')
                .to_owned();
            buffer.drain(..=nl);
            if line.is_empty() {
                continue;
            }
            let runtime = runtime.clone();
            let reply = tokio::task::spawn_blocking(move || runtime.handle_line(&line))
                .await
                .map_err(io::Error::other)?;
            let mut reply = serde_json::to_vec(&reply).map_err(io::Error::other)?;
            reply.push(b'\n');
            tokio::select! {_ = stop.changed()=>return Ok(()),result=timeout(Duration::from_secs(5),pipe.write_all(&reply))=>result.map_err(|_|io::Error::new(io::ErrorKind::TimedOut,"pipe write"))??};
        }
        if buffer.len() > 8 * 1024 * 1024 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "request exceeds 8 MB",
            ));
        }
    }
}
