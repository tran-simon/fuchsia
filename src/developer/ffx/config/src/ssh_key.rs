// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::query;

use anyhow::{anyhow, bail, Context, Result};
use base64::display::Base64Display;
use ring::{
    rand::{self, SystemRandom},
    signature::{Ed25519KeyPair, KeyPair},
};
use std::{
    fs::{DirBuilder, File, OpenOptions},
    io::{BufRead, BufReader, Cursor, Read, Write},
    os::unix::fs::{DirBuilderExt, OpenOptionsExt},
    path::PathBuf,
    str,
};

/// Paths to the private and public SSH keys that are used by ffx.
/// These can be loaded from the configuration keys `ssh.pub`
/// and `ssh.priv` by using SshKeyFiles::load().
/// Typical usage is to load from the configuration and then create the keys
/// if they are missing when initializing a device via flashing/paving
///  or starting an emulator instance.
///
/// let ssh_keys = SshKeyFiles::load().await?;
/// ssh_keys.create_keys_if_needed()?;
///
/// This is preferred since generating the private key when attempting to access
/// a device already initialized is pointless.
///
///
#[derive(Debug, Default)]
pub struct SshKeyFiles {
    pub authorized_keys: PathBuf,
    pub private_key: PathBuf,
}

const KEYTYPE: &[u8] = b"ssh-ed25519";
const COMMENT: &str = "Generated by ffx for Fuchsia";
const AUTH_MAGIC: &[u8] = b"openssh-key-v1\0";

impl SshKeyFiles {
    /// loads the file paths from the config properties `ssh.pub` and `ssh.priv`.
    /// If none of the paths configured are to files that exist, the paths will
    ///  be set to the default locations, which is the first element in the config settings.
    pub async fn load() -> Result<Self> {
        // initialize to the first path in the list, then iterate through the list to select
        // the first file that exists.
        let authorized_keys_files: Vec<PathBuf> = query("ssh.pub").get().await?;
        if authorized_keys_files.is_empty() {
            bail!("No paths configured for `ssh.pub`.");
        }
        let mut authorized_keys = authorized_keys_files[0].to_path_buf();
        for path in &authorized_keys_files {
            if path.exists() {
                authorized_keys = path.to_path_buf();
                break;
            }
        }

        let key_files: Vec<PathBuf> = query("ssh.priv").get().await?;
        if key_files.is_empty() {
            bail!("No paths configured for `ssh.priv`.");
        }
        let mut private_key = key_files[0].to_path_buf();
        for path in &key_files {
            if path.exists() {
                private_key = path.to_path_buf();
                break;
            }
        }
        Ok(SshKeyFiles { authorized_keys, private_key })
    }

    /// Generates the ed25519 key pair and saves openssh authorized_keys and private key file,
    /// if the paths point to files that do not exist.
    pub fn create_keys_if_needed(&self) -> Result<()> {
        let mut public_key: Vec<u8> = vec![];

        // Validate the paths are non-empty
        if self.private_key.display().to_string().is_empty() {
            bail!("private key path cannot be empty");
        }
        if self.authorized_keys.display().to_string().is_empty() {
            bail!("authorized keys path cannot be empty");
        }

        if !self.private_key.exists() {
            // There is no private key, so generate a new key pair
            // resulting in a byte array .der encoded.
            eprintln!("Creating SSH key pair: {}", self.private_key.display());
            let rng = SystemRandom::new();
            let bytes = Ed25519KeyPair::generate_pkcs8(&rng)
                .map_err(|m| anyhow!("could not generate pkcs8 document: {:?}", m))?;
            let key_pair = Ed25519KeyPair::from_pkcs8(bytes.as_ref())
                .map_err(|m| anyhow!("could not get keypair from pkcs8 document: {:?}", m))?;

            write_private_key(&self.private_key, &key_pair, bytes.as_ref(), &rng)?;
            public_key = key_pair.public_key().as_ref().to_vec()
        } else if !self.authorized_keys.exists() {
            // If we get here we need to get the public key from the private key.
            // If reading fails, the file is corrupted or it is not the format expected.
            // The easiest corrective action would be for the user to delete the file or generate authorized keys using ssh-keygen.
            // Since there is no way to know if the key is being used, an error is returned since   there is no safe way to continue
            // and make sure the keys are valid.
            public_key = read_public_key_from_private(&self.private_key)?
        }

        // check to write the authorized keys file for when the private existed, and when it was generated.
        if !self.authorized_keys.exists() {
            write_public_key(&self.authorized_keys, &public_key)?;
        }

        Ok(())
    }
}

/// Formats the key data for the authorized_keys file.
fn get_public_key_data(pubkey: &[u8]) -> Result<Vec<u8>> {
    let mut out_bytes: Vec<u8> = vec![];

    // public key is 2 "cstrings", which are strings with no null terminator preceded by the
    // length.
    write_cstring(&mut out_bytes, KEYTYPE)?;
    write_cstring(&mut out_bytes, pubkey)?;

    Ok(out_bytes)
}

/// Writes the public key information to the authorized_keys file.
fn write_public_key(path: &PathBuf, public_key: &[u8]) -> Result<()> {
    eprintln!("Writing authorized_keys file: {}", path.display());
    let public_key_data = get_public_key_data(public_key)?;
    let pubkey_b64 = Base64Display::with_config(&public_key_data, base64::STANDARD);

    if let Some(parent) = path.parent() {
        DirBuilder::new().recursive(true).mode(0o700).create(parent)?;
    };
    let mut w =
        OpenOptions::new().write(true).read(true).create_new(true).mode(0o600).open(&path)?;
    writeln!(&mut w, "{} {} {}", str::from_utf8(KEYTYPE)?, pubkey_b64, COMMENT)?;
    // File is closed when it goes out of scope.
    Ok(())
}

/// Writes the private key file.
fn write_private_key(
    path: &PathBuf,
    key_pair: &Ed25519KeyPair,
    document: &[u8],
    rng: &SystemRandom,
) -> Result<()> {
    // private key file
    let none = b"none";

    // magic pattern to identify this data, null terminated.
    let mut priv_out_bytes: Vec<u8> = vec![];
    priv_out_bytes.write_all(AUTH_MAGIC)?;

    // ciphername
    write_cstring(&mut priv_out_bytes, none)?;

    // kdfname (none), and length 0
    write_cstring(&mut priv_out_bytes, none)?;
    priv_out_bytes.write_all(&[0, 0, 0, 0])?;

    // number of keys, always 1.
    priv_out_bytes.write_all(&[0, 0, 0, 1])?;

    // public key - this is the same contents as appears in the authorized_keys file.
    let public_key_data = get_public_key_data(key_pair.public_key().as_ref())?;
    write_cstring(&mut priv_out_bytes, &public_key_data)?;

    // private key.
    let mut key_bytes: Vec<u8> = vec![];

    // random u32 checkbytes, write it 2 times.
    let rand_bytes: [u8; 4] = rand::generate(rng).unwrap().expose();
    key_bytes.write_all(&rand_bytes)?;
    key_bytes.write_all(&rand_bytes)?;

    // The type of key.
    write_cstring(&mut key_bytes, KEYTYPE)?;

    // Extract the secret part of the key from the pkcs8 document.
    // the first 16 bytes are the version and algorithm oid. The private
    // key data starts at 16, and is 32 bytes
    // secret key, should be 32 bytes.
    let secret = &document[16..48];

    // pub key 32 bytes.
    write_cstring(&mut key_bytes, key_pair.public_key().as_ref())?;

    // the private key is the secret with the public appended for a
    // total of 64 bytes.
    let mut private_key_data: Vec<u8> = Vec::from(secret);
    private_key_data.extend_from_slice(key_pair.public_key().as_ref());
    write_cstring(&mut key_bytes, &private_key_data)?;

    // add the comment.
    write_cstring(&mut key_bytes, COMMENT.as_bytes())?;

    // padding
    let mut i: u8 = 0;
    while key_bytes.len() % 8 != 0 {
        i += 1;
        key_bytes.write_all(&[i])?;
    }

    write_cstring(&mut priv_out_bytes, &key_bytes)?;

    let begin = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    let end = "-----END OPENSSH PRIVATE KEY-----\n";

    if let Some(parent) = path.parent() {
        DirBuilder::new().recursive(true).mode(0o700).create(parent)?;
    };
    let mut w =
        OpenOptions::new().write(true).read(true).create_new(true).mode(0o600).open(&path)?;
    writeln!(&mut w, "{}", begin)?;
    writeln!(&mut w, "{}", Base64Display::with_config(&priv_out_bytes, base64::STANDARD))?;
    writeln!(&mut w, "{}", end)?;
    // File is closed when it goes out of scope.
    Ok(())
}

/// Reads the public key from the private key file.
fn read_public_key_from_private(path: &PathBuf) -> Result<Vec<u8>> {
    let mut started = false;
    let mut encoded: String = String::from("");
    let priv_key_file = File::open(path)?;
    for line_result in BufReader::new(priv_key_file).lines() {
        let line = line_result?;
        if line.starts_with("----") && line.contains("BEGIN OPENSSH PRIVATE KEY") {
            started = true;
            continue;
        }
        if line.starts_with("----") && line.contains("END OPENSSH PRIVATE KEY") {
            //done
            break;
        }
        if started {
            // append all lines be between begin and end, trimming whitespace.
            encoded.push_str(line.trim());
        }
    }
    // decode the base64 string into bytes.
    let data = base64::decode(&encoded)?;
    let mut buf = Cursor::new(data);

    let mut element: Vec<u8> = vec![];

    // read the magic, it is null terminated.
    buf.read_until(0, &mut element).context("reading key magic header")?;
    if element != AUTH_MAGIC {
        bail!("Invalid private key header {:?}", &element);
    }

    // read cipher and kdf settings, both none.
    element = read_cstring(&mut buf)?;
    if "none" != str::from_utf8(&element)? {
        bail!("Invalid private key header, expected 'none' {:?}", &element);
    }
    element = read_cstring(&mut buf)?;
    if "none" != str::from_utf8(&element)? {
        bail!("Invalid private key header, expected 'none' {:?}", &element);
    }
    let mut u32_bytes = [0u8; 4];
    buf.read_exact(&mut u32_bytes)?;
    if u32::from_be_bytes(u32_bytes) != 0 {
        bail!("Invalid private key header, expected 0, got {:?}", &u32_bytes);
    }

    // read number of keys, should only be 1.
    buf.read_exact(&mut u32_bytes)?;
    if u32::from_be_bytes(u32_bytes) != 1 {
        bail!("Invalid private key count, expected 1, got {:?}", &u32_bytes);
    }

    // read the public key data
    element = read_cstring(&mut buf)?;

    // this is keytype|key. Read the type, then return the key
    let mut keydata = Cursor::new(&element);
    read_cstring(&mut keydata)?;
    let pubkey = read_cstring(&mut keydata)?;

    Ok(pubkey)
}

fn write_cstring(buf: &mut dyn Write, bytes: &[u8]) -> Result<()> {
    let len: u32 = bytes.len().try_into()?;
    buf.write_all(&len.to_be_bytes())?;
    buf.write_all(bytes)?;
    Ok(())
}

fn read_cstring(buf: &mut dyn Read) -> Result<Vec<u8>> {
    let mut size = [0u8; 4];
    buf.read_exact(&mut size)?;
    let len = u32::from_be_bytes(size);
    if len > 0 {
        let sz: usize = len.try_into().unwrap();
        let mut ret: Vec<u8> = vec![0; sz];
        buf.read_exact(&mut ret)?;
        return Ok(ret);
    }
    Ok(vec![])
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::ConfigLevel;
    use serde_json::json;
    use std::fs::{self, File};
    use std::io::Write;
    use tempfile::TempDir;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_load() -> Result<()> {
        // Set up the test environment and set the ssh key paths
        let _env = crate::test_init().await?;
        query("ssh.pub")
            .level(Some(ConfigLevel::User))
            .set(json!(["$ENV_PATH_THAT_IS_NOT_SET", "/expected/default", "someother"]))
            .await?;
        query("ssh.priv")
            .level(Some(ConfigLevel::User))
            .set(json!([
                "$ENV_PATH_THAT_IS_NOT_SET_2",
                "/expected/default/private",
                "someother/place"
            ]))
            .await?;

        // set the config

        let ssh_files = SshKeyFiles::load().await?;
        assert!(&ssh_files.authorized_keys.display().to_string() == "/expected/default");
        assert!(&ssh_files.private_key.display().to_string() == "/expected/default/private");
        Ok(())
    }

    #[test]
    fn test_create_with_existing() -> Result<()> {
        let tmp_dir = TempDir::new()?;

        let auth_key_path = tmp_dir.path().join("authorized_keys");
        let private_path = tmp_dir.path().join("privatekey");

        // scope to force the file to close.
        {
            let mut tmp_file = File::create(&auth_key_path)?;
            let test_private_key = include_str!("../testdata/test1_ed25519");
            tmp_file.write_all(b"unchanged")?;
            let mut priv_file = File::create(&private_path)?;
            priv_file.write_all(test_private_key.as_bytes())?;
        }

        let ssh_files = SshKeyFiles { authorized_keys: auth_key_path, private_key: private_path };
        ssh_files.create_keys_if_needed()?;

        let contents = fs::read_to_string(ssh_files.authorized_keys)?;
        assert!(contents == "unchanged");
        Ok(())
    }

    #[test]
    fn test_create_with_missing_auth_keys() -> Result<()> {
        let tmp_dir = TempDir::new()?;

        let auth_key_path = tmp_dir.path().join("authorized_keys");
        let private_path = tmp_dir.path().join("privatekey");

        // scope to force the file to close.
        {
            let test_private_key = include_str!("../testdata/test1_ed25519");
            let mut priv_file = File::create(&private_path)?;
            priv_file.write_all(test_private_key.as_bytes())?;
        }

        let ssh_files = SshKeyFiles { authorized_keys: auth_key_path, private_key: private_path };
        ssh_files.create_keys_if_needed()?;

        let contents = fs::read_to_string(ssh_files.authorized_keys)?;
        let expected_contents = include_str!("../testdata/test1_authorized_keys");

        assert!(contents == expected_contents);
        Ok(())
    }

    #[test]
    fn test_create_with_missing_keys() -> Result<()> {
        let tmp_dir = TempDir::new()?;

        let auth_key_path = tmp_dir.path().join("authorized_keys");
        let private_path = tmp_dir.path().join("privatekey");

        assert!(!&auth_key_path.exists());
        assert!(!&private_path.exists());

        let ssh_files = SshKeyFiles { authorized_keys: auth_key_path, private_key: private_path };
        ssh_files.create_keys_if_needed()?;

        assert!(&ssh_files.authorized_keys.exists());
        assert!(&ssh_files.private_key.exists());
        Ok(())
    }

    #[test]
    fn test_write_cstring() -> Result<()> {
        let mut data = vec![];
        let mut expected_data: Vec<u8> = vec![0, 0, 0, 5];
        expected_data.extend_from_slice("hello".as_bytes());
        write_cstring(&mut data, "hello".as_bytes())?;

        assert!(data == expected_data);

        let mut input = Cursor::new(data);
        let read_data = read_cstring(&mut input)?;
        assert!(read_data == "hello".as_bytes());
        Ok(())
    }

    #[test]
    fn test_create_with_missing_directory_for_keys() -> Result<()> {
        let tmp_dir = TempDir::new()?;

        let new_dir_path = tmp_dir.path().join("new-dir");
        let auth_key_path = new_dir_path.join("authorized_keys");
        let private_path = new_dir_path.join("privatekey");

        assert!(!&auth_key_path.exists());
        assert!(!&private_path.exists());

        let ssh_files = SshKeyFiles { authorized_keys: auth_key_path, private_key: private_path };
        ssh_files.create_keys_if_needed()?;

        assert!(&ssh_files.authorized_keys.exists());
        assert!(&ssh_files.private_key.exists());
        Ok(())
    }
}
