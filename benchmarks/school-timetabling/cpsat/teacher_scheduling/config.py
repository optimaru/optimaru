from pydantic import Field
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    SOLVER_TIME_LIMIT_SECONDS: float = Field(default=60.0, description="CP-SAT search time limit (seconds)")
    SOLVER_NUM_WORKERS: int = Field(default=8, description="Number of CP-SAT parallel search workers")
    SOLVER_LOG_SEARCH_PROGRESS: bool = Field(default=False, description="Whether to print the CP-SAT search log")

    model_config = SettingsConfigDict(env_file=".env", env_file_encoding="utf-8", extra="ignore")


env = Settings()
