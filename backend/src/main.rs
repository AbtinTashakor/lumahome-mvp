mod api;
mod error;
mod persistence;
mod state;

use std::{env, error::Error, net::SocketAddr};

const DEFAULT_BIND_ADDRESS: &str = "0.0.0.0:3000";

#[tokio::main]
async fn main() {
    if let Err(error) = run().await {
        eprintln!("startup failed: {error}");
        std::process::exit(1);
    }
}

async fn run() -> Result<(), Box<dyn Error>> {
    let address = bind_address()?;
    let app = api::app().await?;
    let listener = tokio::net::TcpListener::bind(address).await?;

    println!("LumaHome backend listening on http://{address}");

    axum::serve(listener, app).await?;
    Ok(())
}

fn bind_address() -> Result<SocketAddr, Box<dyn Error>> {
    let raw =
        env::var("LUMAHOME_BIND_ADDRESS").unwrap_or_else(|_| DEFAULT_BIND_ADDRESS.to_string());
    raw.parse()
        .map_err(|error| format!("invalid LUMAHOME_BIND_ADDRESS '{raw}': {error}").into())
}
