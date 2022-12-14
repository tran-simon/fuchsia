// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Result;
use errors::ffx_bail;
use ffx_core::ffx_plugin;
use ffx_package_archive_extract_args::ExtractCommand;
use ffx_package_archive_utils::{read_file_entries, ArchiveEntry, FarArchiveReader, FarListReader};
use ffx_writer::Writer;
use std::fs;

#[ffx_plugin("ffx_package")]
pub async fn cmd_extract(
    cmd: ExtractCommand,
    #[ffx(machine = Vec<T:Serialize>)] mut writer: Writer,
) -> Result<()> {
    let mut archive_reader: Box<dyn FarListReader> = Box::new(FarArchiveReader::new(&cmd.archive)?);

    extract_implementation(cmd, &mut writer, &mut archive_reader)
}

fn extract_implementation<W: std::io::Write>(
    cmd: ExtractCommand,
    writer: &mut W,
    reader: &mut Box<dyn FarListReader>,
) -> Result<()> {
    let entries = read_file_entries(reader)?;
    let dest_dir = cmd.output_dir;

    let mut extract_list: Vec<ArchiveEntry> = vec![];
    for filepath in cmd.far_paths {
        if let Some(entry) = entries.iter().find(|x| {
            if cmd.as_hash {
                x.path == filepath.to_string_lossy()
            } else {
                x.name == filepath.to_string_lossy()
            }
        }) {
            extract_list.push(entry.clone());
        } else {
            ffx_bail!(
                "file {} not found in {}",
                filepath.to_string_lossy(),
                cmd.archive.to_string_lossy()
            );
        }
    }
    // If no names are on the command line, then extract everything.
    if extract_list.is_empty() {
        extract_list.extend(entries);
        // Sort the list so it is consistent order vs. the order from the iterator.
        extract_list.sort();
    }

    for entry in extract_list {
        let destpath =
            if cmd.as_hash { dest_dir.join(&entry.path) } else { dest_dir.join(&entry.name) };
        if cmd.verbose {
            writeln!(writer, "{}", destpath.to_string_lossy())?;
        }
        let data = reader.read_entry(&entry)?;

        if let Some(p) = destpath.parent() {
            fs::create_dir_all(p)?;
        }
        fs::write(destpath, &data)?;
    }

    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;
    use ffx_package_archive_utils::{
        test_utils::{
            create_mockreader, test_contents, BLOB1, BLOB2, DATA_SOME_FILE_BLOB,
            DATA_SOME_FILE_PATH, LIB_RUN_SO_BLOB, LIB_RUN_SO_PATH, RUN_ME_BLOB, RUN_ME_PATH,
        },
        MockFarListReader,
    };
    use std::{path::PathBuf, str};
    use tempfile::Builder;

    #[test]
    fn test_extract_blob() -> Result<()> {
        let tmp_out_dir = Builder::new().prefix("test_extract").tempdir()?;
        let tmp_out_path = tmp_out_dir.into_path();
        let cmd = ExtractCommand {
            archive: PathBuf::from("some.far"),
            far_paths: vec![PathBuf::from(BLOB1)],
            output_dir: tmp_out_path.clone(),
            as_hash: true,
            verbose: false,
        };

        let mut output: Vec<u8> = vec![];

        let mock_reader: MockFarListReader = create_mockreader();

        let expected_contents = vec![(BLOB1, test_contents(BLOB1))];
        let expected_output = "";

        let mut boxed_reader: Box<dyn FarListReader> = Box::from(mock_reader);

        extract_implementation(cmd, &mut output, &mut boxed_reader)?;
        assert_eq!(expected_output, str::from_utf8(&output)?);

        for (blob, contents) in expected_contents {
            let extracted_path = tmp_out_path.join(blob);
            let actual_contents = fs::read_to_string(extracted_path)?;
            assert_eq!(actual_contents, str::from_utf8(&contents)?);
        }

        Ok(())
    }

    #[test]
    fn test_extract_blob_verbose() -> Result<()> {
        let tmp_out_dir = Builder::new().prefix("test_extract").tempdir()?;
        let tmp_out_path = tmp_out_dir.into_path();

        let cmd = ExtractCommand {
            archive: PathBuf::from("some.far"),
            far_paths: vec![PathBuf::from(BLOB1), PathBuf::from(BLOB2)],
            output_dir: tmp_out_path.clone(),
            as_hash: true,
            verbose: true,
        };

        let mut output: Vec<u8> = vec![];

        let mock_reader: MockFarListReader = create_mockreader();

        let expected_contents = vec![(BLOB1, test_contents(BLOB1)), (BLOB2, test_contents(BLOB2))];
        let expected_output = format!(
            "{}\n{}\n",
            tmp_out_path.join(BLOB1).to_string_lossy(),
            tmp_out_path.join(BLOB2).to_string_lossy()
        );

        let mut boxed_reader: Box<dyn FarListReader> = Box::from(mock_reader);

        extract_implementation(cmd, &mut output, &mut boxed_reader)?;
        assert_eq!(expected_output, str::from_utf8(&output)?);

        for (blob, contents) in expected_contents {
            let extracted_path = tmp_out_path.join(blob);
            let actual_contents = fs::read_to_string(extracted_path)?;
            assert_eq!(actual_contents, str::from_utf8(&contents)?);
        }

        Ok(())
    }

    #[test]
    fn test_extract_filename() -> Result<()> {
        let tmp_out_dir = Builder::new().prefix("test_extract").tempdir()?;
        let tmp_out_path = tmp_out_dir.into_path();

        let cmd = ExtractCommand {
            archive: PathBuf::from("some.far"),
            far_paths: vec![PathBuf::from(LIB_RUN_SO_PATH)],
            as_hash: false,
            output_dir: tmp_out_path.clone(),
            verbose: false,
        };

        let mut output: Vec<u8> = vec![];

        let mock_reader: MockFarListReader = create_mockreader();

        let expected_contents = vec![(LIB_RUN_SO_PATH, test_contents(BLOB2))];
        let expected_output = "";

        let mut boxed_reader: Box<dyn FarListReader> = Box::from(mock_reader);

        extract_implementation(cmd, &mut output, &mut boxed_reader)?;

        assert_eq!(expected_output, str::from_utf8(&output)?);

        for (name, contents) in expected_contents {
            let extracted_path = tmp_out_path.join(name);
            let actual_contents = fs::read_to_string(extracted_path)?;
            assert_eq!(actual_contents, str::from_utf8(&contents)?);
        }

        Ok(())
    }

    #[test]
    fn test_extract_all() -> Result<()> {
        let tmp_out_dir = Builder::new().prefix("test_extract").tempdir()?;
        let tmp_out_path = tmp_out_dir.into_path();

        let cmd = ExtractCommand {
            archive: PathBuf::from("some.far"),
            far_paths: vec![],
            as_hash: false,
            output_dir: tmp_out_path.clone(),
            verbose: true,
        };

        let mut output: Vec<u8> = vec![];

        let mock_reader: MockFarListReader = create_mockreader();

        let expected_contents = vec![
            (RUN_ME_PATH, test_contents(RUN_ME_BLOB)),
            (LIB_RUN_SO_PATH, test_contents(LIB_RUN_SO_BLOB)),
            (DATA_SOME_FILE_PATH, test_contents(DATA_SOME_FILE_BLOB)),
        ];

        let expected_output = format!(
            "{}\n{}\n{}\n{}\n{}\n{}\n{}\n",
            tmp_out_path.join(DATA_SOME_FILE_PATH).to_string_lossy(),
            tmp_out_path.join(LIB_RUN_SO_PATH).to_string_lossy(),
            tmp_out_path.join("meta.far").to_string_lossy(),
            tmp_out_path.join("meta/contents").to_string_lossy(),
            tmp_out_path.join("meta/package").to_string_lossy(),
            tmp_out_path.join("meta/the_component.cm").to_string_lossy(),
            tmp_out_path.join(RUN_ME_PATH).to_string_lossy()
        );

        let mut boxed_reader: Box<dyn FarListReader> = Box::from(mock_reader);

        extract_implementation(cmd, &mut output, &mut boxed_reader)?;

        assert_eq!(expected_output, str::from_utf8(&output)?);

        for (name, contents) in expected_contents {
            let extracted_path = tmp_out_path.join(name);
            let actual_contents = fs::read_to_string(&extracted_path)?;
            assert_eq!(
                actual_contents,
                str::from_utf8(&contents)?,
                "Checking contents for {:?}",
                &extracted_path
            );
        }

        Ok(())
    }
}
