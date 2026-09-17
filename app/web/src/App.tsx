import { useEffect, useState } from 'react';
import { LayoutDashboard, FileSpreadsheet, Download, RefreshCw, ServerCrash, ExternalLink, BarChart2, LineChart as LineChartIcon } from 'lucide-react';
import { ComposedChart, Bar, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';
import { Toaster, toast } from 'sonner';

function App() {
    const [activeTab, setActiveTab] = useState<'dashboard' | 'reports'>('dashboard');
    const [selectedBancada, setSelectedBancada] = useState<string>('Geral');
    const [statusData, setStatusData] = useState<any>(null);
    const [reportsData, setReportsData] = useState<any[]>([]);
    const [loading, setLoading] = useState(true);
    const [loadingReports, setLoadingReports] = useState(false);
    const [isGenerating, setIsGenerating] = useState(false);
    const [chartType, setChartType] = useState<'bar' | 'line'>('bar');
    const [error, setError] = useState<string | null>(null);

    const fetchStatus = async () => {
        try {
            setLoading(true);
            const res = await fetch('http://localhost:8000/api/status');
            if (!res.ok) throw new Error('Falha ao conectar na API');
            const data = await res.json();
            setStatusData(data);
            setError(null);
        } catch (err: any) {
            setError(err.message);
        } finally {
            setLoading(false);
        }
    };

    const fetchReports = async () => {
        try {
            setLoadingReports(true);
            const res = await fetch('http://localhost:8000/api/reports');
            if (!res.ok) throw new Error('Falha ao buscar relatórios');
            const data = await res.json();
            if (data.success) {
                setReportsData(data.reports);
            }
        } catch (err: any) {
            console.error(err);
        } finally {
            setLoadingReports(false);
        }
    };

    const generateReport = async () => {
        setIsGenerating(true);
        try {
            const res = await fetch('http://localhost:8000/api/reports/generate', { method: 'POST' });
            const data = await res.json();
            if (data.success) {
                toast.success('Relatório gerado com sucesso no Google Drive!');
                fetchReports(); // Refresh da lista
            } else {
                toast.error('Erro ao gerar relatório: ' + data.error);
            }
        } catch (err) {
            console.error('Erro ao gerar relatório:', err);
            toast.error('Falha na comunicação com o servidor.');
        } finally {
            setIsGenerating(false);
        }
    };

    useEffect(() => {
        if (activeTab === 'dashboard') {
            fetchStatus();
            const interval = setInterval(fetchStatus, 1000); // Atualiza a cada 1s (Temporário para vídeo)
            return () => clearInterval(interval);
        } else {
            fetchReports();
        }
    }, [activeTab]);

    const formatSize = (bytes: number) => {
        if (!bytes || bytes === 0) return '--';
        return (bytes / 1024).toFixed(2) + ' KB';
    };

    // Renderização do Dashboard
    const renderDashboard = () => {
        if (loading && !statusData) return <div className="p-4">Carregando dados da fábrica...</div>;
        if (error) return (
            <div className="card text-center text-error" style={{ padding: '3rem' }}>
                <ServerCrash size={48} className="mx-auto mb-4" />
                <h3 className="mb-2">Sem conexão com a Fábrica</h3>
                <p>{error}</p>
                <button className="btn btn-outline mt-4" onClick={fetchStatus}>Tentar novamente</button>
            </div>
        );

        const { global_kpis, kpis_bancadas, grafico_hora, grafico_hora_bancadas } = statusData?.data || {};

        // Dados dinâmicos com base na aba selecionada
        const isGeral = selectedBancada === 'Geral';
        const bancadaData = !isGeral ? (kpis_bancadas || []).find((b: any) => b.bancada === selectedBancada) : null;

        const currentKpis = isGeral ? global_kpis : {
            total_pecas: bancadaData?.total_pecas || 0,
            bancadas_ativas: bancadaData?.disponibilidade ? `${bancadaData.disponibilidade}%` : '0%',
            taxa_pecas_hora: bancadaData?.taxa_pecas_hora || 0,
            total_paradas: bancadaData?.paradas || 0
        };

        let currentGrafico = isGeral ? (grafico_hora || []) : (grafico_hora_bancadas?.[selectedBancada] || []);

        // Calcula a média para adicionar ao gráfico
        if (currentGrafico.length > 0) {
            const total = currentGrafico.reduce((acc: number, val: any) => acc + val.pecas, 0);
            const media = Math.round(total / currentGrafico.length);
            currentGrafico = currentGrafico.map((g: any) => ({ ...g, media }));
        }

        return (
            <>
                {/* Abas de Bancada */}
                <div style={{ display: 'flex', gap: '0.5rem', marginBottom: '1.5rem', overflowX: 'auto', paddingBottom: '0.5rem' }}>
                    <button
                        className={`btn ${isGeral ? 'btn-primary' : 'btn-outline'}`}
                        onClick={() => setSelectedBancada('Geral')}
                    >
                        Geral
                    </button>
                    {(kpis_bancadas || []).map((b: any) => (
                        <button
                            key={b.bancada}
                            className={`btn ${selectedBancada === b.bancada ? 'btn-primary' : 'btn-outline'}`}
                            onClick={() => setSelectedBancada(b.bancada)}
                        >
                            Bancada {b.bancada}
                        </button>
                    ))}
                </div>

                <div className="card-grid">
                    <div className="card">
                        <div className="card-title">
                            Total peças
                            <LayoutDashboard size={16} />
                        </div>
                        <div className="card-value text-success">
                            {currentKpis?.total_pecas || 0}
                        </div>
                        <div className="card-subtext">{isGeral ? 'Peças na fábrica toda' : 'Peças contadas'}</div>
                    </div>
                    <div className="card">
                        <div className="card-title">
                            {isGeral ? 'Bancadas Ativas' : 'Disponibilidade'}
                        </div>
                        <div className="card-value">
                            {currentKpis?.bancadas_ativas || 0}
                        </div>
                        <div className="card-subtext">{isGeral ? 'Postos comunicando' : '% Tempo operando'}</div>
                    </div>
                    <div className="card">
                        <div className="card-title">
                            {isGeral ? 'Ritmo Médio' : 'Ritmo (Peças/h)'}
                        </div>
                        <div className="card-value text-primary">
                            {isGeral ? (currentKpis?.taxa_pecas_hora?.toFixed(1) || '0.0') : (currentKpis?.taxa_pecas_hora?.toFixed(1) || '0.0')}
                        </div>
                        <div className="card-subtext">{isGeral ? 'Peças por hora' : 'Velocidade da máquina'}</div>
                    </div>
                    <div className="card">
                        <div className="card-title">
                            {isGeral ? 'Paradas Operacionais' : 'Paradas'}
                        </div>
                        <div className="card-value text-error">
                            {currentKpis?.total_paradas || 0}
                        </div>
                        <div className="card-subtext">{isGeral ? "Paradas > 15 minutos" : "Total de ocorrências"}</div>
                    </div>
                </div>

                <div className="card" style={{ marginBottom: '2rem' }}>
                    <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '1rem' }}>
                        <h3 style={{ color: 'var(--text-main)', fontSize: '1.125rem', margin: 0 }}>Evolução da Produção Horária {isGeral ? '' : `(${selectedBancada})`}</h3>
                        <div style={{ display: 'flex', gap: '0.25rem', background: '#f1f5f9', padding: '0.25rem', borderRadius: '0.5rem' }}>
                            <button
                                onClick={() => setChartType('bar')}
                                style={{ background: chartType === 'bar' ? 'white' : 'transparent', border: 'none', padding: '0.25rem 0.5rem', borderRadius: '0.25rem', cursor: 'pointer', display: 'flex', alignItems: 'center', boxShadow: chartType === 'bar' ? '0 1px 3px rgba(0,0,0,0.1)' : 'none' }}
                                title="Gráfico de Barras"
                            >
                                <BarChart2 size={16} color={chartType === 'bar' ? '#3b82f6' : '#64748b'} />
                            </button>
                            <button
                                onClick={() => setChartType('line')}
                                style={{ background: chartType === 'line' ? 'white' : 'transparent', border: 'none', padding: '0.25rem 0.5rem', borderRadius: '0.25rem', cursor: 'pointer', display: 'flex', alignItems: 'center', boxShadow: chartType === 'line' ? '0 1px 3px rgba(0,0,0,0.1)' : 'none' }}
                                title="Gráfico de Linhas"
                            >
                                <LineChartIcon size={16} color={chartType === 'line' ? '#3b82f6' : '#64748b'} />
                            </button>
                        </div>
                    </div>
                    <div style={{ height: '300px' }}>
                        <ResponsiveContainer width="100%" height="100%">
                            <ComposedChart data={currentGrafico} margin={{ top: 20, right: 20, bottom: 20, left: 20 }}>
                                <CartesianGrid strokeDasharray="3 3" vertical={false} stroke="#e2e8f0" />
                                <XAxis dataKey="hora" axisLine={false} tickLine={false} tick={{ fill: '#64748b' }} dy={10} />
                                <YAxis axisLine={false} tickLine={false} tick={{ fill: '#64748b' }} dx={-10} />
                                <Tooltip
                                    contentStyle={{ borderRadius: '8px', border: '1px solid #e2e8f0', boxShadow: '0 4px 6px -1px rgb(0 0 0 / 0.1)' }}
                                    formatter={(value: any, name: string) => {
                                        if (name === "pecas") return [`${value} peças`, "Produção"];
                                        if (name === "media") return [`${value} peças`, "Média Horária"];
                                        return [value, name];
                                    }}
                                    labelFormatter={(label) => {
                                        if (typeof label === 'string' && label.includes(':')) {
                                            const parts = label.split(':');
                                            const hour = parts[0];
                                            const min = parseInt(parts[1], 10);
                                            return `${hour}:${min.toString().padStart(2, '0')}:00 - ${hour}:${(min + 9).toString().padStart(2, '0')}:59`;
                                        }
                                        return label;
                                    }}
                                />
                                {chartType === 'bar' ? (
                                    <Bar dataKey="pecas" fill="#3b82f6" radius={[4, 4, 0, 0]} maxBarSize={50} name="pecas" />
                                ) : (
                                    <Line type="monotone" dataKey="pecas" stroke="#3b82f6" strokeWidth={3} dot={{ r: 4 }} activeDot={{ r: 6 }} name="pecas" />
                                )}
                                <Line type="stepAfter" dataKey="media" stroke="#f59e0b" strokeWidth={2} strokeDasharray="5 5" dot={false} name="media" />
                            </ComposedChart>
                        </ResponsiveContainer>
                    </div>
                </div>

                {isGeral && (
                    <div className="table-container" style={{ marginBottom: '2rem' }}>
                        <div className="table-header">
                            <h3 style={{ fontSize: '1.125rem', fontWeight: 600 }}>Desempenho por Bancada</h3>
                        </div>
                        <table>
                            <thead>
                                <tr>
                                    <th>Bancada</th>
                                    <th style={{ textAlign: 'right' }}>Total Peças</th>
                                    <th style={{ textAlign: 'right' }}>Disponibilidade</th>
                                    <th style={{ textAlign: 'right' }}>Tempo Parado</th>
                                    <th style={{ textAlign: 'right' }}>{"Paradas (>15m)"}</th>
                                </tr>
                            </thead>
                            <tbody>
                                {(!kpis_bancadas || kpis_bancadas.length === 0) ? (
                                    <tr>
                                        <td colSpan={5} style={{ textAlign: 'center', color: 'var(--text-muted)' }}>
                                            Nenhuma bancada registrou produção ainda.
                                        </td>
                                    </tr>
                                ) : (
                                    kpis_bancadas.map((bancada: any, idx: number) => (
                                        <tr key={idx}>
                                            <td style={{ fontWeight: 600 }}>{bancada.bancada}</td>
                                            <td style={{ textAlign: 'right' }}>{bancada.total_pecas}</td>
                                            <td style={{ textAlign: 'right' }}>
                                                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'flex-end', gap: '0.5rem' }}>
                                                    <span style={{ fontSize: '0.875rem' }}>{bancada.disponibilidade.toFixed(1)}%</span>
                                                    <div style={{ width: '60px', height: '6px', backgroundColor: '#e2e8f0', borderRadius: '3px', overflow: 'hidden' }}>
                                                        <div style={{
                                                            height: '100%',
                                                            width: `${Math.min(100, Math.max(0, bancada.disponibilidade))}%`,
                                                            backgroundColor: bancada.disponibilidade >= 90 ? '#10b981' : (bancada.disponibilidade >= 75 ? '#f59e0b' : '#ef4444')
                                                        }} />
                                                    </div>
                                                </div>
                                            </td>
                                            <td style={{ textAlign: 'right' }}>{bancada.tempo_parado.toFixed(1)} min</td>
                                            <td style={{ textAlign: 'right', color: bancada.paradas > 0 ? 'var(--color-error)' : 'inherit', fontWeight: bancada.paradas > 0 ? '600' : 'normal' }}>
                                                {bancada.paradas}
                                            </td>
                                        </tr>
                                    ))
                                )}
                            </tbody>
                        </table>
                    </div>
                )}
            </>
        );
    };

    // Renderização da Central de Relatórios
    const renderReports = () => {
        return (
            <div className="table-container">
                <div className="table-header">
                    <h3 style={{ fontSize: '1.125rem', fontWeight: 600 }}>Relatórios no Google Drive</h3>
                    <div style={{ display: 'flex', gap: '0.5rem' }}>
                        <button className="btn btn-primary" onClick={generateReport} disabled={isGenerating}>
                            {isGenerating ? 'Gerando (Aguarde)...' : 'Gerar Relatório Agora'}
                        </button>
                        <button className="btn btn-outline" onClick={fetchReports}>
                            <RefreshCw size={16} /> Atualizar
                        </button>
                    </div>
                </div>
                <table>
                    <thead>
                        <tr>
                            <th>Nome do Arquivo</th>
                            <th>Tamanho</th>
                            <th>Data de Criação</th>
                            <th style={{ textAlign: 'right' }}>Ações</th>
                        </tr>
                    </thead>
                    <tbody>
                        {loadingReports ? (
                            <tr>
                                <td colSpan={4} style={{ textAlign: 'center', color: 'var(--text-muted)' }}>
                                    Buscando relatórios...
                                </td>
                            </tr>
                        ) : reportsData.length === 0 ? (
                            <tr>
                                <td colSpan={4} style={{ textAlign: 'center', color: 'var(--text-muted)' }}>
                                    Nenhum relatório encontrado.
                                </td>
                            </tr>
                        ) : (
                            reportsData.map((report, idx) => (
                                <tr key={idx}>
                                    <td style={{ fontWeight: 500 }}>
                                        <div style={{ display: 'flex', alignItems: 'center', gap: '0.5rem' }}>
                                            <FileSpreadsheet size={16} className="text-success" />
                                            {report.filename}
                                        </div>
                                    </td>
                                    <td>{formatSize(report.size_bytes)}</td>
                                    <td>{new Date(report.created_at).toLocaleString('pt-BR')}</td>
                                    <td style={{ textAlign: 'right' }}>
                                        <button className="btn btn-primary" onClick={() => window.open(report.webViewLink, '_blank')}>
                                            <ExternalLink size={16} /> Abrir
                                        </button>
                                    </td>
                                </tr>
                            ))
                        )}
                    </tbody>
                </table>
            </div>
        );
    };

    return (
        <div className="app-container">
            {/* Sidebar */}
            <aside className="sidebar">
                <div>
                    <h1>EdgeBench</h1>
                    <p style={{ padding: '0 1rem', fontSize: '0.75rem', color: 'var(--text-muted)', marginTop: '0.25rem' }}>
                        Painel Executivo
                    </p>
                </div>

                <nav className="nav-links">
                    <div
                        className={`nav-link ${activeTab === 'dashboard' ? 'active' : ''}`}
                        onClick={() => setActiveTab('dashboard')}
                    >
                        <LayoutDashboard size={18} />
                        Visão Geral
                    </div>
                    <div
                        className={`nav-link ${activeTab === 'reports' ? 'active' : ''}`}
                        onClick={() => setActiveTab('reports')}
                    >
                        <FileSpreadsheet size={18} />
                        Central de Relatórios
                    </div>
                </nav>
            </aside>

            {/* Main Content */}
            <main className="main-content">
                <header className="header">
                    <h2>{activeTab === 'dashboard' ? 'Painel de Produção Tempo Real' : 'Central de Relatórios e Downloads'}</h2>

                    <div style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
                        {activeTab === 'dashboard' && (
                            <>
                                <span style={{ fontSize: '0.875rem', color: 'var(--text-muted)' }}>
                                    Última atualização: {new Date().toLocaleTimeString('pt-BR')}
                                </span>
                                <button className="btn btn-outline" style={{ padding: '0.5rem 1rem', fontSize: '0.875rem' }} onClick={fetchStatus}>
                                    <RefreshCw size={14} /> Atualizar
                                </button>
                            </>
                        )}
                    </div>
                </header>

                {activeTab === 'dashboard' ? renderDashboard() : renderReports()}

            </main>
            <Toaster position="bottom-right" richColors />
        </div>
    );
}

export default App;
