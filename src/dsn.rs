//! Minimal parser for Firebird connection strings.
//!
//! Format: `firebird://user:password@host:port/database?charset=UTF8`
//!
//! Unlike a strict URL parser, the database portion is taken verbatim so
//! Windows paths (`C:\...`) and Unix paths (`/var/lib/...`) survive intact, and
//! IPv6 hosts are supported via bracket notation (`[fd7a:115c::1]:3050`).

use std::error::Error;

#[derive(Debug, Default, Clone)]
pub struct Dsn {
    pub user: Option<String>,
    pub password: Option<String>,
    pub host: Option<String>,
    pub port: Option<u16>,
    pub database: Option<String>,
    pub charset: Option<String>,
}

impl Dsn {
    pub fn parse(input: &str) -> Result<Dsn, Box<dyn Error>> {
        let mut s = input.trim();
        for prefix in ["firebird://", "fb://"] {
            if let Some(rest) = s.strip_prefix(prefix) {
                s = rest;
                break;
            }
        }

        let mut dsn = Dsn::default();

        // Split off the query string (charset, etc.).
        let (main, query) = match s.split_once('?') {
            Some((m, q)) => (m, Some(q)),
            None => (s, None),
        };

        // authority / database. The database may itself contain '/' or '\',
        // so we only split on the first '/'.
        let (authority, database) = match main.split_once('/') {
            Some((a, d)) => (a, Some(d)),
            None => (main, None),
        };
        if let Some(db) = database {
            if !db.is_empty() {
                dsn.database = Some(db.to_string());
            }
        }

        // credentials @ hostport
        let (creds, hostport) = match authority.rsplit_once('@') {
            Some((c, hp)) => (Some(c), hp),
            None => (None, authority),
        };

        if let Some(creds) = creds {
            match creds.split_once(':') {
                Some((u, p)) => {
                    if !u.is_empty() {
                        dsn.user = Some(u.to_string());
                    }
                    dsn.password = Some(p.to_string());
                }
                None => {
                    if !creds.is_empty() {
                        dsn.user = Some(creds.to_string());
                    }
                }
            }
        }

        let (host, port) = parse_hostport(hostport)?;
        dsn.host = host;
        dsn.port = port;

        if let Some(query) = query {
            for pair in query.split('&') {
                if let Some((k, v)) = pair.split_once('=') {
                    if k.eq_ignore_ascii_case("charset") {
                        dsn.charset = Some(v.to_string());
                    }
                }
            }
        }

        Ok(dsn)
    }
}

fn parse_hostport(hostport: &str) -> Result<(Option<String>, Option<u16>), Box<dyn Error>> {
    if hostport.is_empty() {
        return Ok((None, None));
    }

    // Bracketed IPv6: [addr] or [addr]:port
    if let Some(rest) = hostport.strip_prefix('[') {
        let close = rest
            .find(']')
            .ok_or("malformed IPv6 host in DSN: missing ']'")?;
        let host = rest[..close].to_string();
        let after = &rest[close + 1..];
        let port = match after.strip_prefix(':') {
            Some(p) if !p.is_empty() => Some(parse_port(p)?),
            _ => None,
        };
        return Ok((Some(host), port));
    }

    // host or host:port (hostnames/IPv4 contain no ':')
    match hostport.rsplit_once(':') {
        Some((h, p)) if !p.is_empty() => Ok((opt(h), Some(parse_port(p)?))),
        _ => Ok((opt(hostport), None)),
    }
}

fn parse_port(p: &str) -> Result<u16, Box<dyn Error>> {
    p.parse::<u16>()
        .map_err(|_| format!("invalid port in DSN: {p}").into())
}

fn opt(s: &str) -> Option<String> {
    if s.is_empty() {
        None
    } else {
        Some(s.to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_full_dsn_with_ipv6_and_windows_path() {
        let d = Dsn::parse(
            "firebird://archmax_readonly:t34mw37k_1!@[fd7a:115c:a1e0::7f38:6a39]:3050/C:\\Teamwerk\\Vertec\\DB\\VERTEC.fdb?charset=UTF8",
        )
        .unwrap();
        assert_eq!(d.user.as_deref(), Some("archmax_readonly"));
        assert_eq!(d.password.as_deref(), Some("t34mw37k_1!"));
        assert_eq!(d.host.as_deref(), Some("fd7a:115c:a1e0::7f38:6a39"));
        assert_eq!(d.port, Some(3050));
        assert_eq!(d.database.as_deref(), Some("C:\\Teamwerk\\Vertec\\DB\\VERTEC.fdb"));
        assert_eq!(d.charset.as_deref(), Some("UTF8"));
    }

    #[test]
    fn parses_ipv4_host_port_unix_path() {
        let d = Dsn::parse("firebird://SYSDBA:masterkey@127.0.0.1:3050/var/lib/firebird/data/test.fdb").unwrap();
        assert_eq!(d.host.as_deref(), Some("127.0.0.1"));
        assert_eq!(d.port, Some(3050));
        assert_eq!(d.database.as_deref(), Some("var/lib/firebird/data/test.fdb"));
    }

    #[test]
    fn parses_without_scheme_or_creds() {
        let d = Dsn::parse("localhost/test.fdb").unwrap();
        assert_eq!(d.host.as_deref(), Some("localhost"));
        assert_eq!(d.port, None);
        assert_eq!(d.database.as_deref(), Some("test.fdb"));
        assert_eq!(d.user, None);
    }
}
