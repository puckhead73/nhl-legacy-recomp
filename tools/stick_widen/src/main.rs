//! stick_widen — widen the bit-packed `stick` equipment index in nhlng.db.
//!
//! EA's TDB format is self-describing: each table stores per-field descriptors
//! carrying (record_bit_offset, bit_width), and records are bit-packed. This
//! tool widens the `stick` field from 7 bits (0..127) to 8 bits (0..255) by:
//!   1. bit-shifting every record's bits above the field up by 1 (opaque blob
//!      shift — preserves all downstream field VALUES, string or int),
//!   2. bumping the field descriptor's bit_width and every later field's
//!      record_bit_offset by 1, and record_length_bits by 1,
//!   3. writing a test value (e.g. 200) into the now-8-bit field, then
//!   4. resealing all CRCs via tdb_core::write_tdb.
//!
//! Constraint: the +1 bit must fit in the record's existing byte padding
//! (record_length_bits + 1 <= record_length_bytes * 8); otherwise the record
//! stride would grow, which needs whole-table relocation (out of scope here).
//!
//! Usage:
//!   stick_widen inspect <db> [table_id]
//!   stick_widen widen   <in.db> <out.db> [field_id=xFXw] [value=200] [limit=all]

use std::env;
use std::fs;

use tdb_core::{
    write_bits, write_tdb, Endian, TdbFile, DIRECTORY_ENTRY_SIZE, FIELD_DESCRIPTOR_SIZE,
    HEADER_SIZE, TABLE_HEADER_SIZE,
};

fn main() {
    let args: Vec<String> = env::args().collect();
    let r = match args.get(1).map(String::as_str) {
        Some("inspect") => inspect(&args),
        Some("widen") => widen(&args),
        Some("cmprec") => cmprec(&args),
        Some("vals") => vals(&args),
        Some("setval") => setval(&args),
        _ => {
            eprintln!(
                "usage:\n  stick_widen inspect <db> [table_id]\n  \
                 stick_widen widen <in.db> <out.db> [field_id=xFXw] [value=200] [limit=all]"
            );
            std::process::exit(2);
        }
    };
    if let Err(e) = r {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}

/// Read/write a single bit at an absolute bit index, honoring the file's bit
/// convention: LE => bit 0 is the LSB of byte 0; BE => bit 0 is the MSB of
/// byte 0. (Mirrors tdb_core::bitview.)
fn get_bit(buf: &[u8], bit: usize, endian: Endian) -> bool {
    let mask = match endian {
        Endian::Little => 1u8 << (bit % 8),
        Endian::Big => 0x80u8 >> (bit % 8),
    };
    (buf[bit / 8] & mask) != 0
}

fn set_bit(buf: &mut [u8], bit: usize, val: bool, endian: Endian) {
    let mask = match endian {
        Endian::Little => 1u8 << (bit % 8),
        Endian::Big => 0x80u8 >> (bit % 8),
    };
    if val {
        buf[bit / 8] |= mask;
    } else {
        buf[bit / 8] &= !mask;
    }
}

/// Read a `width`-bit field at `bit_offset`, honoring endianness. Mirrors
/// tdb_core::bitview::BitView::read for both conventions.
fn read_bits(buf: &[u8], bit_offset: usize, width: u32, endian: Endian) -> u64 {
    let mut val = 0u64;
    for k in 0..width as usize {
        let bit = get_bit(buf, bit_offset + k, endian) as u64;
        match endian {
            Endian::Little => val |= bit << k,
            Endian::Big => val |= bit << (width as usize - 1 - k),
        }
    }
    val
}

fn data_region(file: &TdbFile, ti: usize) -> (usize, usize) {
    let n = file.tables.len();
    let table_data_start = HEADER_SIZE + n * DIRECTORY_ENTRY_SIZE;
    let t = &file.tables[ti];
    let hp = table_data_start + t.data_offset as usize;
    let data_start = hp + TABLE_HEADER_SIZE + (t.header.num_fields as usize) * FIELD_DESCRIPTOR_SIZE;
    let h = &t.header;
    let data_end = match h.data_allocation_type {
        2 | 6 | 10 | 14 => data_start + (h.record_length_bytes as usize) * (h.max_records as usize),
        34 | 66 => {
            // next table absolute (sorted by data_offset), else file end - 4
            let cur = t.data_offset;
            let next = file
                .tables
                .iter()
                .map(|x| x.data_offset)
                .filter(|&o| o > cur)
                .min();
            match next {
                Some(off) => table_data_start + off as usize,
                None => file.source_size.saturating_sub(4),
            }
        }
        _ => data_start + (h.record_length_bytes as usize) * (h.current_records as usize),
    };
    (data_start, data_end)
}

fn inspect(args: &[String]) -> Result<(), String> {
    let path = args.get(2).ok_or("inspect: need <db>")?;
    let filter = args.get(3).map(String::as_str);
    let bytes = fs::read(path).map_err(|e| format!("read {path}: {e}"))?;
    let file = TdbFile::parse(&bytes).map_err(|e| format!("parse: {e:?}"))?;
    println!(
        "{}: endian={:?} tables={} db_size={} file={}B",
        path, file.header.endian, file.header.num_tables, file.header.db_size, bytes.len()
    );
    if filter == Some("--list") {
        for (ti, t) in file.tables.iter().enumerate() {
            println!(
                "  [{ti:>3}] '{}'  fields={:>2} rec_bytes={} rec_bits={} cur={} max={}",
                t.id.as_str().unwrap_or("????"),
                t.header.num_fields,
                t.header.record_length_bytes,
                t.header.record_length_bits,
                t.header.current_records,
                t.header.max_records
            );
        }
        return Ok(());
    }
    for (ti, t) in file.tables.iter().enumerate() {
        let tid = t.id.as_str().unwrap_or("????");
        let has_xfxw = t.fields.iter().any(|f| f.id.as_str() == Some("xFXw"));
        let show = match filter {
            // Match a table id OR any table that contains a field with this id.
            Some(f) => tid == f || t.fields.iter().any(|fd| fd.id.as_str() == Some(f)),
            None => tid == "ajmx" || has_xfxw,
        };
        if !show {
            continue;
        }
        let h = &t.header;
        let (ds, de) = data_region(&file, ti);
        let phys = if h.record_length_bytes > 0 {
            (de - ds) / h.record_length_bytes as usize
        } else {
            0
        };
        println!(
            "\n== table '{tid}' (idx {ti}) alloc_type={} rec_bytes={} rec_bits={} pad_bits={} \
             cur={} max={} fields={} phys_records={} ==",
            h.data_allocation_type,
            h.record_length_bytes,
            h.record_length_bits,
            h.record_length_bytes as i64 * 8 - h.record_length_bits as i64,
            h.current_records,
            h.max_records,
            h.num_fields,
            phys
        );
        let mut fields: Vec<_> = t.fields.iter().collect();
        fields.sort_by_key(|f| f.record_bit_offset);
        for f in fields {
            let id = f.id.as_str().unwrap_or("????");
            let star = if id == "xFXw" { "  <== stick?" } else { "" };
            println!(
                "   off {:>4}  w {:>3}  kind {:>2}  id '{}'{}",
                f.record_bit_offset, f.bit_width, f.kind_code, id, star
            );
        }
    }
    Ok(())
}

/// Compare one record's every field value across two DBs (orig vs widened),
/// proving the bit-insert preserved all non-stick fields.
fn cmprec(args: &[String]) -> Result<(), String> {
    let a = args.get(2).ok_or("cmprec: need <db_a>")?;
    let b = args.get(3).ok_or("cmprec: need <db_b>")?;
    let table_id = args.get(4).map(String::as_str).unwrap_or("xmja");
    let rec: usize = args.get(5).and_then(|s| s.parse().ok()).unwrap_or(0);

    let read_one = |p: &str| -> Result<(Vec<u8>, TdbFile, usize), String> {
        let by = fs::read(p).map_err(|e| format!("read {p}: {e}"))?;
        let f = TdbFile::parse(&by).map_err(|e| format!("parse {p}: {e:?}"))?;
        let ti = (0..f.tables.len())
            .find(|&i| f.tables[i].id.as_str() == Some(table_id))
            .ok_or(format!("table '{table_id}' not in {p}"))?;
        Ok((by, f, ti))
    };
    let (ba, fa, tia) = read_one(a)?;
    let (bb, fb, tib) = read_one(b)?;
    let ea = fa.header.endian;
    let eb = fb.header.endian;
    let (dsa, _) = data_region(&fa, tia);
    let (dsb, _) = data_region(&fb, tib);
    let rlba = fa.tables[tia].header.record_length_bytes as usize;
    let rlbb = fb.tables[tib].header.record_length_bytes as usize;

    println!("record {rec} of '{table_id}': {a} vs {b}");
    let mut diffs = 0;
    for f in &fa.tables[tia].fields {
        let id = f.id.as_str().unwrap_or("????");
        let va = read_bits(&ba, (dsa + rec * rlba) * 8 + f.record_bit_offset as usize, f.bit_width, ea);
        let fbf = fb.tables[tib].fields.iter().find(|x| x.id == f.id).unwrap();
        let vb = read_bits(&bb, (dsb + rec * rlbb) * 8 + fbf.record_bit_offset as usize, fbf.bit_width, eb);
        let mark = if va != vb { diffs += 1; "  <<< DIFF" } else { "" };
        println!("   '{id}': {va} -> {vb}{mark}");
    }
    println!("{diffs} field(s) changed");
    Ok(())
}

/// Dump a field's value distribution across all records of a table — to find
/// which wXFx table actually holds the per-player stick assignments.
fn vals(args: &[String]) -> Result<(), String> {
    let path = args.get(2).ok_or("vals: need <db>")?;
    let table_id = args.get(3).map(String::as_str).unwrap_or("xmja");
    let field_id = args.get(4).map(String::as_str).unwrap_or("wXFx");
    let bytes = fs::read(path).map_err(|e| format!("read {path}: {e}"))?;
    let file = TdbFile::parse(&bytes).map_err(|e| format!("parse: {e:?}"))?;
    let endian = file.header.endian;
    let ti = (0..file.tables.len())
        .find(|&i| file.tables[i].id.as_str() == Some(table_id))
        .ok_or(format!("table '{table_id}' not found"))?;
    let f = file.tables[ti]
        .fields
        .iter()
        .find(|f| f.id.as_str() == Some(field_id))
        .ok_or(format!("field '{field_id}' not in '{table_id}'"))?;
    let (ds, de) = data_region(&file, ti);
    let rlb = file.tables[ti].header.record_length_bytes as usize;
    let nrec = (de - ds) / rlb;
    let mut hist: std::collections::BTreeMap<u64, usize> = std::collections::BTreeMap::new();
    for r in 0..nrec {
        let v = read_bits(&bytes, (ds + r * rlb) * 8 + f.record_bit_offset as usize, f.bit_width, endian);
        *hist.entry(v).or_default() += 1;
    }
    println!("'{table_id}.{field_id}' (w{}) over {nrec} records — value: count", f.bit_width);
    for (v, c) in &hist {
        println!("   {v}: {c}");
    }
    println!("distinct values: {}", hist.len());
    Ok(())
}

/// CONTROL: set a field to `value` in every record of a table WITHOUT widening
/// (value must fit the existing field width). Tests whether the game even reads
/// our edits for this table — no bit-insert, minimal risk.
fn setval(args: &[String]) -> Result<(), String> {
    let in_path = args.get(2).ok_or("setval: need <in.db>")?;
    let out_path = args.get(3).ok_or("setval: need <out.db>")?;
    let table_id = args.get(4).map(String::as_str).unwrap_or("xmja");
    let field_id = args.get(5).map(String::as_str).unwrap_or("wXFx");
    let value: u64 = args.get(6).and_then(|s| s.parse().ok()).ok_or("setval: need <value>")?;

    let bytes = fs::read(in_path).map_err(|e| format!("read {in_path}: {e}"))?;
    let file = TdbFile::parse(&bytes).map_err(|e| format!("parse: {e:?}"))?;
    let endian = file.header.endian;
    let mut out = bytes.clone();

    let ti = (0..file.tables.len())
        .find(|&i| file.tables[i].id.as_str() == Some(table_id))
        .ok_or(format!("table '{table_id}' not found"))?;
    let f = file.tables[ti].fields.iter().find(|f| f.id.as_str() == Some(field_id))
        .ok_or(format!("field '{field_id}' not in '{table_id}'"))?;
    let off = f.record_bit_offset;
    let w = f.bit_width;
    if value >= (1u64 << w) {
        return Err(format!("value {value} does not fit in {w} bits"));
    }
    let rlb = file.tables[ti].header.record_length_bytes as usize;
    let (ds, de) = data_region(&file, ti);
    let nrec = (de - ds) / rlb;
    for r in 0..nrec {
        let abs = ((ds + r * rlb) * 8 + off as usize) as u32;
        if !write_bits(&mut out, abs, w, value, endian) {
            return Err(format!("write_bits failed at record {r}"));
        }
    }
    let sealed = write_tdb(&file, &out);
    fs::write(out_path, &sealed).map_err(|e| format!("write {out_path}: {e}"))?;
    let rf = TdbFile::parse(&sealed).map_err(|e| format!("reparse: {e:?}"))?;
    let report = tdb_core::validate_crcs(&sealed, &rf);
    println!(
        "setval '{table_id}.{field_id}' = {value} in {nrec} records (no widen); \
         wrote {out_path}; CRC invalid_count={}",
        report.invalid_count()
    );
    Ok(())
}

fn widen(args: &[String]) -> Result<(), String> {
    let in_path = args.get(2).ok_or("widen: need <in.db>")?;
    let out_path = args.get(3).ok_or("widen: need <out.db>")?;
    let table_id = args.get(4).map(String::as_str).unwrap_or("xmja");
    let field_id = args.get(5).map(String::as_str).unwrap_or("wXFx");
    let value: u64 = args.get(6).and_then(|s| s.parse().ok()).unwrap_or(200);
    let limit: usize = args.get(7).and_then(|s| s.parse().ok()).unwrap_or(usize::MAX);

    let bytes = fs::read(in_path).map_err(|e| format!("read {in_path}: {e}"))?;
    let mut file = TdbFile::parse(&bytes).map_err(|e| format!("parse: {e:?}"))?;
    let endian = file.header.endian;
    let mut out = bytes.clone();

    let ti = (0..file.tables.len())
        .find(|&i| file.tables[i].id.as_str() == Some(table_id))
        .ok_or(format!("table '{table_id}' not found"))?;
    let fi = file.tables[ti]
        .fields
        .iter()
        .position(|f| f.id.as_str() == Some(field_id))
        .ok_or(format!("field '{field_id}' not in table '{table_id}'"))?;

    let old_off = file.tables[ti].fields[fi].record_bit_offset as usize;
    let old_w = file.tables[ti].fields[fi].bit_width;
    // Where the new (zero) bit is inserted so the field's VALUE is preserved:
    //   BE (MSB-first): new MSB at the field FRONT  -> insert at old_off.
    //   LE (LSB-first): new MSB at the field BACK    -> insert at old_off+old_w.
    // (Inserting on the wrong side shifts the value by a factor of 2.)
    let insert_pos = match endian {
        Endian::Big => old_off,
        Endian::Little => old_off + old_w as usize,
    };
    let rlb = file.tables[ti].header.record_length_bytes as usize;
    let rbits = file.tables[ti].header.record_length_bits as usize;

    if old_w != 7 {
        return Err(format!("field '{field_id}' is {old_w} bits, expected 7"));
    }
    if rbits + 1 > rlb * 8 {
        return Err(format!(
            "no padding room (rec_bits={rbits}, rec_bytes={rlb}); widening would grow \
             stride (needs table relocation, out of scope)"
        ));
    }
    if value >= 256 {
        return Err(format!("value {value} does not fit in 8 bits"));
    }

    let (ds, de) = data_region(&file, ti);
    let nrec = (de - ds) / rlb;

    // 1. Per-record opaque bit-insert: open a free (zero) bit at `insert_pos`
    //    by moving bits [insert_pos, rbits) up by one. Endian-aware bit-blob
    //    shift, so every other field's VALUE is preserved regardless of type.
    for r in 0..nrec {
        let base = (ds + r * rlb) * 8;
        for b in (insert_pos..rbits).rev() {
            let v = get_bit(&out, base + b, endian);
            set_bit(&mut out, base + b + 1, v, endian);
        }
        set_bit(&mut out, base + insert_pos, false, endian); // the new bit = 0
    }

    // 2. Descriptor + header updates: widen the field (offset unchanged), shift
    //    every other field whose data sits at/after the insert, bump rec bits.
    {
        let t = &mut file.tables[ti];
        t.fields[fi].bit_width = old_w + 1;
        for f in t.fields.iter_mut() {
            if (f.record_bit_offset as usize) >= insert_pos && f.id.as_str() != Some(field_id) {
                f.record_bit_offset += 1;
            }
        }
        t.header.record_length_bits += 1;
    }

    // 3. Set the test value in the first `limit` records.
    let set_n = limit.min(nrec);
    for r in 0..set_n {
        let abs_bit = ((ds + r * rlb) * 8 + old_off) as u32;
        if !write_bits(&mut out, abs_bit, old_w + 1, value, endian) {
            return Err(format!("write_bits failed at record {r}"));
        }
    }
    println!(
        "table '{table_id}' field '{field_id}': widened {old_w}->{} bits at off {old_off}; \
         shifted {nrec} records; set value={value} in {set_n} records",
        old_w + 1
    );

    let sealed = write_tdb(&file, &out);
    fs::write(out_path, &sealed).map_err(|e| format!("write {out_path}: {e}"))?;
    println!("wrote {} ({} bytes)", out_path, sealed.len());

    // 4. Verify: reparse, read back the field width + record 0/1 values.
    let rf = TdbFile::parse(&sealed).map_err(|e| format!("reparse: {e:?}"))?;
    let rti = (0..rf.tables.len())
        .find(|&i| rf.tables[i].id.as_str() == Some(table_id))
        .unwrap();
    let f = rf.tables[rti].fields.iter().find(|f| f.id.as_str() == Some(field_id)).unwrap();
    let (rds, _) = data_region(&rf, rti);
    let v0 = read_bits(&sealed, rds * 8 + f.record_bit_offset as usize, f.bit_width, endian);
    println!(
        "verify '{table_id}.{field_id}': width={} record0={} (expected {value})",
        f.bit_width, v0
    );
    let report = tdb_core::validate_crcs(&sealed, &rf);
    println!("CRC invalid_count = {}", report.invalid_count());
    // Only assert the set value when we actually set record 0 (limit>0).
    if set_n > 0 && v0 != value.min(255) {
        return Err("verification mismatch: record0 did not read back the set value".into());
    }
    Ok(())
}
