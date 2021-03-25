// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context, Result},
    async_std::{
        io::BufReader,
        path::PathBuf,
        sync::{Arc, Mutex},
    },
    ffx_config::{get, get_sdk},
    fuchsia_async::Task,
    futures::{
        channel::mpsc::{Receiver, Sender},
        AsyncBufReadExt, AsyncWriteExt, FutureExt, SinkExt, StreamExt,
    },
    smol::Async,
    std::process::{Child, Command, Stdio},
};

const BARRIER: &str = "<ffx symbolizer>\n";

pub fn is_symbolizer_context_marker(s: &str) -> bool {
    return s.starts_with("{{{reset}}}")
        || s.starts_with("{{{bt")
        || s.starts_with("{{{mmap")
        || s.starts_with("{{{dumpfile")
        || s.starts_with("{{{module");
}

fn symbol_list_contains(output: String, abs_path: PathBuf) -> bool {
    output.lines().filter_map(|l| l.split("\t").nth(1)).any(|d| PathBuf::from(d) == abs_path)
}

pub async fn is_current_sdk_root_registered() -> Result<bool> {
    let sdk_root: PathBuf = get("sdk.root").await?;

    let path = get_sdk().await?.get_host_tool("symbol-index")?;
    let c = Command::new(path).arg("list").output()?;
    let abs_path = sdk_root.canonicalize().await.unwrap();

    Ok(symbol_list_contains(String::from_utf8(c.stdout).unwrap(), abs_path))
}

#[async_trait::async_trait]
pub trait Symbolizer {
    async fn start(
        &self,
        rx: Receiver<String>,
        tx: Sender<String>,
        extra_args: Vec<String>,
    ) -> Result<()>;
}

struct LogSymbolizerInner {
    _child: Child,
    _task: Task<Result<()>>,
}
pub struct LogSymbolizer {
    inner: Arc<Mutex<Option<LogSymbolizerInner>>>,
}

impl LogSymbolizer {
    pub fn new() -> Self {
        Self { inner: Arc::new(Mutex::new(None)) }
    }
}

#[async_trait::async_trait]
impl<'a> Symbolizer for LogSymbolizer {
    async fn start(
        &self,
        mut rx: Receiver<String>,
        mut tx: Sender<String>,
        extra_args: Vec<String>,
    ) -> Result<()> {
        let path = get_sdk()
            .await?
            .get_host_tool("symbolizer")
            .context("getting symbolizer binary path")?;
        let mut c = Command::new(path)
            .args(vec![
                "--symbol-server",
                "gs://fuchsia-artifacts/debug",
                "--symbol-server",
                "gs://fuchsia-artifacts-internal/debug",
                "--symbol-server",
                "gs://fuchsia-artifacts-release/debug",
            ])
            .args(extra_args)
            .stdout(Stdio::piped())
            .stdin(Stdio::piped())
            .stderr(Stdio::inherit())
            .spawn()
            .context("Spawning symbolizer")?;
        let stdout = Async::new(c.stdout.take().context("missing stdout")?)?;
        let mut stdin = Async::new(c.stdin.take().context("missing stdin")?)?;

        let mut inner = self.inner.lock().await;
        inner.replace(LogSymbolizerInner {
            _child: c,
            _task: Task::spawn(async move {
                let mut stdout_reader = BufReader::new(stdout);
                loop {
                    let mut msg = match rx.next().await {
                        Some(s) => s,
                        None => {
                            log::warn!("input stream is now empty");
                            break;
                        }
                    };

                    msg.push_str(BARRIER);

                    match stdin.write_all(msg.as_bytes()).await {
                        Ok(_) => {},
                        Err(e) => {
                            log::warn!("writing to symbolizer stdin failed: {}", e);
                            continue;
                        }
                    }

                    let mut stdout_buf = String::default();
                    match stdout_reader.read_line(&mut stdout_buf).await {
                        Ok(_) => {},
                        Err(e) => {
                            log::warn!("reading from symbolizer stdout failed: {}", e);
                            continue;
                        }
                    }

                    if stdout_buf.as_str() == BARRIER {
                        tx.send("\n".to_string()).await?;
                        continue;
                    }

                    let mut result = String::default();
                    while stdout_buf.as_str() != BARRIER {
                        result.push_str(&stdout_buf);
                        stdout_buf = String::default();
                        stdout_reader.read_line(&mut stdout_buf).map(|r| {
                            if let Err(e) = r {
                                log::warn!("got error trying to write to symbolizer output channel: {}", e);
                            }
                            ()
                        }).await;

                    }
                    tx.send(result).map(|r| {
                        if let Err(e) = r {
                            log::warn!("got error trying to write to symbolizer output channel: {}", e);
                        }
                        ()
                    }).await;
                }
                Ok(())
            })
        });
        Ok(())
    }
}

/// A fake symbolizer that simply prepends a fixed string to every line passed to it.
/// As the name implies, it should only be used in tests.
pub struct FakeSymbolizerForTest {
    prefix: String,
    expected_args: Vec<String>,
    _task: Mutex<Option<Task<()>>>,
}

impl FakeSymbolizerForTest {
    pub fn new(prefix: &str, expected_args: Vec<String>) -> Self {
        Self { prefix: prefix.to_string(), expected_args, _task: Mutex::new(None) }
    }
}

#[async_trait::async_trait]
impl Symbolizer for FakeSymbolizerForTest {
    async fn start(
        &self,
        mut rx: Receiver<String>,
        mut tx: Sender<String>,
        extra_args: Vec<String>,
    ) -> Result<()> {
        assert_eq!(
            extra_args, self.expected_args,
            "got wrong args in symbolizer start. got: {:?}, expected: {:?}",
            extra_args, self.expected_args
        );

        let prefix = self.prefix.clone();
        let mut t = self._task.lock().await;
        t.replace(Task::spawn(async move {
            while let Some(out) = rx.next().await {
                if !is_symbolizer_context_marker(&out) {
                    tx.send(format!("{}", out)).await.unwrap();
                    continue;
                }
                let mut new_line = prefix.clone();
                new_line.push_str(&out);
                tx.send(new_line).await.unwrap();
            }
        }));
        Ok(())
    }
}

#[cfg(test)]
mod test {
    use {super::*, futures::channel::mpsc::channel};
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_fake_symbolizer() {
        let args = vec!["arg".to_string(), "arg2".to_string()];
        let s = FakeSymbolizerForTest::new("prefix", args.clone());

        let (mut in_tx, in_rx) = channel(1);
        let (out_tx, mut out_rx) = channel(1);

        s.start(in_rx, out_tx, args.clone()).await.unwrap();

        in_tx.send("{{{reset}}}\n".to_string()).await.unwrap();
        let out = out_rx.next().await.unwrap();
        assert_eq!(out, "prefix{{{reset}}}\n");

        in_tx.send("{{{mmap:something}}\n".to_string()).await.unwrap();
        let out = out_rx.next().await.unwrap();
        assert_eq!(out, "prefix{{{mmap:something}}\n");

        in_tx.send("not_real\n".to_string()).await.unwrap();
        let out = out_rx.next().await.unwrap();
        assert_eq!(out, "not_real\n");
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_symbol_index_list() {
        let input = "
        /home/test/my-symbol-index/.build-id\t/home/test/my-symbol-index
        /some/path
        /some/other/path/.build-id\t/some/other/path"
            .to_string();

        assert!(symbol_list_contains(input.clone(), PathBuf::from("/home/test/my-symbol-index")));
        assert!(symbol_list_contains(input.clone(), PathBuf::from("/some/other/path")));
        assert!(!symbol_list_contains(input.clone(), PathBuf::from("/doesnt/exist")));
    }
}
