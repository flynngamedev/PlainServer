//! `plsproject.json` schema -- the manifest `pls new` creates and
//! `pls build` / `pls run` read back.

use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ServerConfig {
    #[serde(default = "default_port")]
    pub port: u16,
    #[serde(default = "default_tickrate")]
    pub tickrate: u32,
    #[serde(default = "default_max_players")]
    pub max_players: u32,
}

fn default_port() -> u16 {
    8080
}
fn default_tickrate() -> u32 {
    60
}
fn default_max_players() -> u32 {
    64
}

impl Default for ServerConfig {
    fn default() -> Self {
        ServerConfig {
            port: default_port(),
            tickrate: default_tickrate(),
            max_players: default_max_players(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PlsProject {
    pub name: String,
    #[serde(default = "default_version")]
    pub version: String,
    #[serde(default = "default_entry")]
    pub entry: String,
    #[serde(default)]
    pub server: ServerConfig,
}

fn default_version() -> String {
    "0.1.0".to_string()
}
fn default_entry() -> String {
    "main.pls".to_string()
}

impl PlsProject {
    pub fn new(name: &str) -> Self {
        PlsProject {
            name: name.to_string(),
            version: default_version(),
            entry: default_entry(),
            server: ServerConfig::default(),
        }
    }

    pub fn load(path: &Path) -> Result<Self, String> {
        let text = fs::read_to_string(path)
            .map_err(|e| format!("could not read '{}': {}", path.display(), e))?;
        serde_json::from_str(&text).map_err(|e| format!("invalid plsproject.json: {}", e))
    }

    pub fn save(&self, path: &Path) -> Result<(), String> {
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        fs::write(path, text + "\n")
            .map_err(|e| format!("could not write '{}': {}", path.display(), e))
    }
}
